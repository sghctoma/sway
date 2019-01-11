#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/input.h"
#include "swaybar/tray/host.h"
#include "swaybar/tray/icon.h"
#include "swaybar/tray/item.h"
#include "swaybar/tray/tray.h"
#include "background-image.h"
#include "cairo.h"
#include "list.h"
#include "log.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

// TODO menu

static bool sni_ready(struct swaybar_sni *sni) {
	return sni->status && (sni->status[0] == 'N' ? // NeedsAttention
			sni->attention_icon_name || sni->attention_icon_pixmap :
			sni->icon_name || sni->icon_pixmap);
}

static void set_sni_dirty(struct swaybar_sni *sni) {
	if (sni_ready(sni)) {
		sni->min_size = sni->max_size = 0; // invalidate previous icon
		set_bar_dirty(sni->tray->bar);
	}
}

static int read_pixmap(sd_bus_message *msg, struct swaybar_sni *sni,
		const char *prop, list_t **dest) {
	int ret = sd_bus_message_enter_container(msg, 'a', "(iiay)");
	if (ret < 0) {
		wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
		return ret;
	}

	if (sd_bus_message_at_end(msg, 0)) {
		wlr_log(WLR_DEBUG, "%s %s no. of icons = 0", sni->watcher_id, prop);
		return ret;
	}

	list_t *pixmaps = create_list();
	if (!pixmaps) {
		return -12; // -ENOMEM
	}

	while (!sd_bus_message_at_end(msg, 0)) {
		ret = sd_bus_message_enter_container(msg, 'r', "iiay");
		if (ret < 0) {
			wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
			goto error;
		}

		int size;
		ret = sd_bus_message_read(msg, "ii", NULL, &size);
		if (ret < 0) {
			wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
			goto error;
		}

		const void *pixels;
		size_t npixels;
		ret = sd_bus_message_read_array(msg, 'y', &pixels, &npixels);
		if (ret < 0) {
			wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
			goto error;
		}

		struct swaybar_pixmap *pixmap =
			malloc(sizeof(struct swaybar_pixmap) + npixels);
		pixmap->size = size;
		memcpy(pixmap->pixels, pixels, npixels);
		list_add(pixmaps, pixmap);

		sd_bus_message_exit_container(msg);
	}
	list_free_items_and_destroy(*dest);
	*dest = pixmaps;
	wlr_log(WLR_DEBUG, "%s %s no. of icons = %d", sni->watcher_id, prop,
			pixmaps->length);

	return ret;
error:
	list_free_items_and_destroy(pixmaps);
	return ret;
}

struct get_property_data {
	struct swaybar_sni *sni;
	const char *prop;
	const char *type;
	void *dest;
};

static int get_property_callback(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct get_property_data *d = data;
	struct swaybar_sni *sni = d->sni;
	const char *prop = d->prop;
	const char *type = d->type;
	void *dest = d->dest;

	int ret;
	if (sd_bus_message_is_method_error(msg, NULL)) {
		wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop,
				sd_bus_message_get_error(msg)->message);
		ret = sd_bus_message_get_errno(msg);
		goto cleanup;
	}

	ret = sd_bus_message_enter_container(msg, 'v', type);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
		goto cleanup;
	}

	if (!type) {
		ret = read_pixmap(msg, sni, prop, dest);
		if (ret < 0) {
			goto cleanup;
		}
	} else {
		if (*type == 's' || *type == 'o') {
			free(*(char **)dest);
		}

		ret = sd_bus_message_read(msg, type, dest);
		if (ret < 0) {
			wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
			goto cleanup;
		}

		if (*type == 's' || *type == 'o') {
			char **str = dest;
			*str = strdup(*str);
			wlr_log(WLR_DEBUG, "%s %s = '%s'", sni->watcher_id, prop, *str);
		} else if (*type == 'b') {
			wlr_log(WLR_DEBUG, "%s %s = %s", sni->watcher_id, prop,
					*(bool *)dest ? "true" : "false");
		}
	}

	if (strcmp(prop, "Status") == 0 || (sni->status && (sni->status[0] == 'N' ?
				prop[0] == 'A' : strncmp(prop, "Icon", 4) == 0))) {
		set_sni_dirty(sni);
	}
cleanup:
	free(data);
	return ret;
}

static void sni_get_property_async(struct swaybar_sni *sni, const char *prop,
		const char *type, void *dest) {
	struct get_property_data *data = malloc(sizeof(struct get_property_data));
	data->sni = sni;
	data->prop = prop;
	data->type = type;
	data->dest = dest;
	int ret = sd_bus_call_method_async(sni->tray->bus, NULL, sni->service,
			sni->path, "org.freedesktop.DBus.Properties", "Get",
			get_property_callback, data, "ss", sni->interface, prop);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "%s %s: %s", sni->watcher_id, prop, strerror(-ret));
	}
}

/*
 * There is a quirk in sd-bus that in some systems, it is unable to get the
 * well-known names on the bus, so it cannot identify if an incoming signal,
 * which uses the sender's unique name, actually matches the callback's matching
 * sender if the callback uses a well-known name, in which case it just calls
 * the callback and hopes for the best, resulting in false positives. In the
 * case of NewIcon & NewAttentionIcon, this doesn't affect anything, but it
 * means that for NewStatus, if the SNI does not definitely match the sender,
 * then the safe thing to do is to query the status independently.
 * This function returns 1 if the SNI definitely matches the signal sender,
 * which is returned by the calling function to indicate that signal matching
 * can stop since it has already found the required callback, otherwise, it
 * returns 0, which allows matching to continue.
 */
static int sni_check_msg_sender(struct swaybar_sni *sni, sd_bus_message *msg,
		const char *signal) {
	bool has_well_known_names =
		sd_bus_creds_get_mask(sd_bus_message_get_creds(msg)) & SD_BUS_CREDS_WELL_KNOWN_NAMES;
	if (sni->service[0] == ':' || has_well_known_names) {
		wlr_log(WLR_DEBUG, "%s has new %s", sni->watcher_id, signal);
		return 1;
	} else {
		wlr_log(WLR_DEBUG, "%s may have new %s", sni->watcher_id, signal);
		return 0;
	}
}

static int handle_new_icon(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
	sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);
	return sni_check_msg_sender(sni, msg, "icon");
}

static int handle_new_attention_icon(sd_bus_message *msg, void *data,
		sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
	sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);
	return sni_check_msg_sender(sni, msg, "attention icon");
}

static int handle_new_status(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct swaybar_sni *sni = data;
	int ret = sni_check_msg_sender(sni, msg, "status");
	if (ret == 1) {
		char *status;
		int r = sd_bus_message_read(msg, "s", &status);
		if (r < 0) {
			wlr_log(WLR_ERROR, "%s new status error: %s", sni->watcher_id, strerror(-ret));
			ret = r;
		} else {
			free(sni->status);
			sni->status = strdup(status);
			wlr_log(WLR_DEBUG, "%s has new status = '%s'", sni->watcher_id, status);
			set_sni_dirty(sni);
		}
	} else {
		sni_get_property_async(sni, "Status", "s", &sni->status);
	}

	return ret;
}

static void sni_match_signal(struct swaybar_sni *sni, sd_bus_slot **slot,
		char *signal, sd_bus_message_handler_t callback) {
	int ret = sd_bus_match_signal(sni->tray->bus, slot, sni->service, sni->path,
			sni->interface, signal, callback, sni);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Failed to subscribe to signal %s: %s", signal,
				strerror(-ret));
	}
}

struct swaybar_sni *create_sni(char *id, struct swaybar_tray *tray) {
	struct swaybar_sni *sni = calloc(1, sizeof(struct swaybar_sni));
	if (!sni) {
		return NULL;
	}
	sni->tray = tray;
	sni->watcher_id = strdup(id);
	char *path_ptr = strchr(id, '/');
	if (!path_ptr) {
		sni->service = strdup(id);
		sni->path = strdup("/StatusNotifierItem");
		sni->interface = "org.freedesktop.StatusNotifierItem";
	} else {
		sni->service = strndup(id, path_ptr - id);
		sni->path = strdup(path_ptr);
		sni->interface = "org.kde.StatusNotifierItem";
		sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
	}

	// Ignored: Category, Id, Title, WindowId, OverlayIconName,
	//          OverlayIconPixmap, AttentionMovieName, ToolTip
	sni_get_property_async(sni, "Status", "s", &sni->status);
	sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
	sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);
	sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
	sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);
	sni_get_property_async(sni, "ItemIsMenu", "b", &sni->item_is_menu);
	sni_get_property_async(sni, "Menu", "o", &sni->menu);

	sni_match_signal(sni, &sni->new_icon_slot, "NewIcon", handle_new_icon);
	sni_match_signal(sni, &sni->new_attention_icon_slot, "NewAttentionIcon",
			handle_new_attention_icon);
	sni_match_signal(sni, &sni->new_status_slot, "NewStatus", handle_new_status);

	return sni;
}

void destroy_sni(struct swaybar_sni *sni) {
	if (!sni) {
		return;
	}

	sd_bus_slot_unref(sni->new_icon_slot);
	sd_bus_slot_unref(sni->new_attention_icon_slot);
	sd_bus_slot_unref(sni->new_status_slot);

	free(sni->watcher_id);
	free(sni->service);
	free(sni->path);
	free(sni->status);
	free(sni->icon_name);
	free(sni->icon_pixmap);
	free(sni->attention_icon_name);
	free(sni->menu);
	free(sni);
}

static void handle_click(struct swaybar_sni *sni, int x, int y,
		enum x11_button button, int delta) {
	const char *method = sni->tray->bar->config->tray_bindings[button];
	if (!method) {
		static const char *default_bindings[10] = {
			"nop",
			"Activate",
			"SecondaryActivate",
			"ContextMenu",
			"ScrollUp",
			"ScrollDown",
			"ScrollLeft",
			"ScrollRight",
			"nop",
			"nop"
		};
		method = default_bindings[button];
	}
	if (strcmp(method, "nop") == 0) {
		return;
	}
	if (sni->item_is_menu && strcmp(method, "Activate") == 0) {
		method = "ContextMenu";
	}

	if (strncmp(method, "Scroll", strlen("Scroll")) == 0) {
		char dir = method[strlen("Scroll")];
		char *orientation = (dir = 'U' || dir == 'D') ? "vertical" : "horizontal";
		int sign = (dir == 'U' || dir == 'L') ? -1 : 1;

		sd_bus_call_method_async(sni->tray->bus, NULL, sni->service, sni->path,
				sni->interface, "Scroll", NULL, NULL, "is", delta*sign, orientation);
	} else {
		sd_bus_call_method_async(sni->tray->bus, NULL, sni->service, sni->path,
				sni->interface, method, NULL, NULL, "ii", x, y);
	}
}

static int cmp_sni_id(const void *item, const void *cmp_to) {
	const struct swaybar_sni *sni = item;
	return strcmp(sni->watcher_id, cmp_to);
}

static enum hotspot_event_handling icon_hotspot_callback(
		struct swaybar_output *output, struct swaybar_hotspot *hotspot,
		int x, int y, enum x11_button button, void *data) {
	wlr_log(WLR_DEBUG, "Clicked on %s", (char *)data);

	struct swaybar_tray *tray = output->bar->tray;
	int idx = list_seq_find(tray->items, cmp_sni_id, data);

	if (idx != -1) {
		struct swaybar_sni *sni = tray->items->items[idx];
		// guess global position since wayland doesn't expose it
		struct swaybar_config *config = tray->bar->config;
		int global_x = output->output_x + config->gaps.left + x;
		bool top_bar = config->position & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
		int global_y = output->output_y + (top_bar ? config->gaps.top + y:
				(int) output->output_height - config->gaps.bottom - y);

		wlr_log(WLR_DEBUG, "Guessing click position at (%d, %d)", global_x, global_y);
		handle_click(sni, global_x, global_y, button, 1); // TODO get delta from event
		return HOTSPOT_IGNORE;
	} else {
		wlr_log(WLR_DEBUG, "but it doesn't exist");
	}

	return HOTSPOT_PROCESS;
}

uint32_t render_sni(cairo_t *cairo, struct swaybar_output *output, double *x,
		struct swaybar_sni *sni) {
	uint32_t height = output->height * output->scale;
	int padding = output->bar->config->tray_padding;
	int ideal_size = height - 2*padding;
	if ((ideal_size < sni->min_size || ideal_size > sni->max_size) && sni_ready(sni)) {
		bool icon_found = false;
		char *icon_name = sni->status[0] == 'N' ?
			sni->attention_icon_name : sni->icon_name;
		if (icon_name) {
			char *icon_path = find_icon(sni->tray->themes, sni->tray->basedirs,
					icon_name, ideal_size, output->bar->config->icon_theme,
					&sni->min_size, &sni->max_size);
			if (!icon_path && sni->icon_theme_path) {
				icon_path = find_icon_in_dir(icon_name, sni->icon_theme_path,
						&sni->min_size, &sni->max_size);
			}
			if (icon_path) {
				cairo_surface_destroy(sni->icon);
				sni->icon = load_background_image(icon_path);
				free(icon_path);
				icon_found = true;
			}
		}
		if (!icon_found) {
			list_t *pixmaps = sni->status[0] == 'N' ?
				sni->attention_icon_pixmap : sni->icon_pixmap;
			if (pixmaps) {
				int idx = -1;
				unsigned smallest_error = -1; // UINT_MAX
				for (int i = 0; i < pixmaps->length; ++i) {
					struct swaybar_pixmap *pixmap = pixmaps->items[i];
					unsigned error = (ideal_size - pixmap->size) *
						(ideal_size < pixmap->size ? -1 : 1);
					if (error < smallest_error) {
						smallest_error = error;
						idx = i;
					}
				}
				struct swaybar_pixmap *pixmap = pixmaps->items[idx];
				cairo_surface_destroy(sni->icon);
				sni->icon = cairo_image_surface_create_for_data(pixmap->pixels,
						CAIRO_FORMAT_ARGB32, pixmap->size, pixmap->size,
						cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pixmap->size));
			}
		}
	}

	int icon_size;
	cairo_surface_t *icon;
	if (sni->icon) {
		int actual_size = cairo_image_surface_get_height(sni->icon);
		icon_size = actual_size < ideal_size ?
			actual_size*(ideal_size/actual_size) : ideal_size;
		icon = cairo_image_surface_scale(sni->icon, icon_size, icon_size);
	} else { // draw a :(
		icon_size = ideal_size*0.8;
		icon = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, icon_size, icon_size);
		cairo_t *cairo_icon = cairo_create(icon);
		cairo_set_source_u32(cairo_icon, 0xFF0000FF);
		cairo_translate(cairo_icon, icon_size/2, icon_size/2);
		cairo_scale(cairo_icon, icon_size/2, icon_size/2);
		cairo_arc(cairo_icon, 0, 0, 1, 0, 7);
		cairo_fill(cairo_icon);
		cairo_set_operator(cairo_icon, CAIRO_OPERATOR_CLEAR);
		cairo_arc(cairo_icon, 0.35, -0.3, 0.1, 0, 7);
		cairo_fill(cairo_icon);
		cairo_arc(cairo_icon, -0.35, -0.3, 0.1, 0, 7);
		cairo_fill(cairo_icon);
		cairo_arc(cairo_icon, 0, 0.75, 0.5, 3.71238898038469, 5.71238898038469);
		cairo_set_line_width(cairo_icon, 0.1);
		cairo_stroke(cairo_icon);
		cairo_destroy(cairo_icon);
	}

	int padded_size = icon_size + 2*padding;
	*x -= padded_size;
	int y = floor((height - padded_size) / 2.0);

	cairo_operator_t op = cairo_get_operator(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	cairo_set_source_surface(cairo, icon, *x + padding, y + padding);
	cairo_rectangle(cairo, *x, y, padded_size, padded_size);
	cairo_fill(cairo);
	cairo_set_operator(cairo, op);

	cairo_surface_destroy(icon);

	struct swaybar_hotspot *hotspot = calloc(1, sizeof(struct swaybar_hotspot));
	hotspot->x = *x;
	hotspot->y = 0;
	hotspot->width = height;
	hotspot->height = height;
	hotspot->callback = icon_hotspot_callback;
	hotspot->destroy = free;
	hotspot->data = strdup(sni->watcher_id);
	wl_list_insert(&output->hotspots, &hotspot->link);

	return output->height;
}
