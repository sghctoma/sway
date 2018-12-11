#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include "sway/config.h"
#include "sway/output.h"
#include "sway/tree/root.h"
#include "log.h"

int output_name_cmp(const void *item, const void *data) {
	const struct output_config *output = item;
	const char *name = data;

	return strcmp(output->name, name);
}

void output_get_identifier(char *identifier, size_t len,
		struct sway_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	snprintf(identifier, len, "%s %s %s", wlr_output->make, wlr_output->model,
		wlr_output->serial);
}

struct output_config *new_output_config(const char *name) {
	struct output_config *oc = calloc(1, sizeof(struct output_config));
	if (oc == NULL) {
		return NULL;
	}
	oc->name = strdup(name);
	if (oc->name == NULL) {
		free(oc);
		return NULL;
	}
	oc->enabled = -1;
	oc->width = oc->height = -1;
	oc->refresh_rate = -1;
	oc->x = oc->y = -1;
	oc->scale = -1;
	oc->transform = -1;
	return oc;
}

void merge_output_config(struct output_config *dst, struct output_config *src) {
	if (src->enabled != -1) {
		dst->enabled = src->enabled;
	}
	if (src->width != -1) {
		dst->width = src->width;
	}
	if (src->height != -1) {
		dst->height = src->height;
	}
	if (src->x != -1) {
		dst->x = src->x;
	}
	if (src->y != -1) {
		dst->y = src->y;
	}
	if (src->scale != -1) {
		dst->scale = src->scale;
	}
	if (src->refresh_rate != -1) {
		dst->refresh_rate = src->refresh_rate;
	}
	if (src->transform != -1) {
		dst->transform = src->transform;
	}
	if (src->background) {
		free(dst->background);
		dst->background = strdup(src->background);
	}
	if (src->background_option) {
		free(dst->background_option);
		dst->background_option = strdup(src->background_option);
	}
	if (src->background_fallback) {
		free(dst->background_fallback);
		dst->background_fallback = strdup(src->background_fallback);
	}
	if (src->dpms_state != 0) {
		dst->dpms_state = src->dpms_state;
	}
}

static void merge_wildcard_on_all(struct output_config *wildcard) {
	for (int i = 0; i < config->output_configs->length; i++) {
		struct output_config *oc = config->output_configs->items[i];
		if (strcmp(wildcard->name, oc->name) != 0) {
			wlr_log(WLR_DEBUG, "Merging output * config on %s", oc->name);
			merge_output_config(oc, wildcard);
		}
	}
}

struct output_config *store_output_config(struct output_config *oc) {
	bool wildcard = strcmp(oc->name, "*") == 0;
	if (wildcard) {
		merge_wildcard_on_all(oc);
	}

	int i = list_seq_find(config->output_configs, output_name_cmp, oc->name);
	if (i >= 0) {
		wlr_log(WLR_DEBUG, "Merging on top of existing output config");
		struct output_config *current = config->output_configs->items[i];
		merge_output_config(current, oc);
		free_output_config(oc);
		oc = current;
	} else if (!wildcard) {
		wlr_log(WLR_DEBUG, "Adding non-wildcard output config");
		i = list_seq_find(config->output_configs, output_name_cmp, "*");
		if (i >= 0) {
			wlr_log(WLR_DEBUG, "Merging on top of output * config");
			struct output_config *current = new_output_config(oc->name);
			merge_output_config(current, config->output_configs->items[i]);
			merge_output_config(current, oc);
			free_output_config(oc);
			oc = current;
		}
		list_add(config->output_configs, oc);
	} else {
		// New wildcard config. Just add it
		wlr_log(WLR_DEBUG, "Adding output * config");
		list_add(config->output_configs, oc);
	}

	wlr_log(WLR_DEBUG, "Config stored for output %s (enabled: %d) (%dx%d@%fHz "
		"position %d,%d scale %f transform %d) (bg %s %s) (dpms %d)",
		oc->name, oc->enabled, oc->width, oc->height, oc->refresh_rate,
		oc->x, oc->y, oc->scale, oc->transform, oc->background,
		oc->background_option, oc->dpms_state);

	return oc;
}

static void set_mode(struct wlr_output *output, int width, int height,
		float refresh_rate) {
	int mhz = (int)(refresh_rate * 1000);
	if (wl_list_empty(&output->modes)) {
		wlr_log(WLR_DEBUG, "Assigning custom mode to %s", output->name);
		wlr_output_set_custom_mode(output, width, height, mhz);
		return;
	}

	struct wlr_output_mode *mode, *best = NULL;
	wl_list_for_each(mode, &output->modes, link) {
		if (mode->width == width && mode->height == height) {
			if (mode->refresh == mhz) {
				best = mode;
				break;
			}
			best = mode;
		}
	}
	if (!best) {
		wlr_log(WLR_ERROR, "Configured mode for %s not available", output->name);
	} else {
		wlr_log(WLR_DEBUG, "Assigning configured mode to %s", output->name);
		wlr_output_set_mode(output, best);
	}
}

void terminate_swaybg(pid_t pid) {
	int ret = kill(pid, SIGTERM);
	if (ret != 0) {
		wlr_log(WLR_ERROR, "Unable to terminate swaybg [pid: %d]", pid);
	} else {
		int status;
		waitpid(pid, &status, 0);
	}
}

void apply_output_config(struct output_config *oc, struct sway_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;

	if (oc && oc->enabled == 0) {
		if (output->enabled) {
			if (output->bg_pid != 0) {
				terminate_swaybg(output->bg_pid);
				output->bg_pid = 0;
			}
			output_disable(output);
			wlr_output_layout_remove(root->output_layout, wlr_output);
		}
		wlr_output_enable(wlr_output, false);
		return;
	} else if (!output->enabled) {
		if (!oc || oc->dpms_state != DPMS_OFF) {
			wlr_output_enable(wlr_output, true);
		}
		output_enable(output, oc);
		return;
	}

	if (oc && oc->width > 0 && oc->height > 0) {
		wlr_log(WLR_DEBUG, "Set %s mode to %dx%d (%f GHz)", oc->name, oc->width,
			oc->height, oc->refresh_rate);
		set_mode(wlr_output, oc->width, oc->height, oc->refresh_rate);
	} else if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode =
			wl_container_of(wlr_output->modes.prev, mode, link);
		wlr_output_set_mode(wlr_output, mode);
	}
	if (oc && oc->scale > 0) {
		wlr_log(WLR_DEBUG, "Set %s scale to %f", oc->name, oc->scale);
		wlr_output_set_scale(wlr_output, oc->scale);
	}
	if (oc && oc->transform >= 0) {
		wlr_log(WLR_DEBUG, "Set %s transform to %d", oc->name, oc->transform);
		wlr_output_set_transform(wlr_output, oc->transform);
	}

	// Find position for it
	if (oc && (oc->x != -1 || oc->y != -1)) {
		wlr_log(WLR_DEBUG, "Set %s position to %d, %d", oc->name, oc->x, oc->y);
		wlr_output_layout_add(root->output_layout, wlr_output, oc->x, oc->y);
	} else {
		wlr_output_layout_add_auto(root->output_layout, wlr_output);
	}

	int output_i;
	for (output_i = 0; output_i < root->outputs->length; ++output_i) {
		if (root->outputs->items[output_i] == output) {
			break;
		}
	}

	if (output->bg_pid != 0) {
		terminate_swaybg(output->bg_pid);
	}
	if (oc && oc->background && config->swaybg_command) {
		wlr_log(WLR_DEBUG, "Setting background for output %d to %s",
				output_i, oc->background);

		size_t len = snprintf(NULL, 0, "%s %d \"%s\" %s %s",
				config->swaybg_command, output_i, oc->background,
				oc->background_option,
				oc->background_fallback ? oc->background_fallback : "");
		char *command = malloc(len + 1);
		if (!command) {
			wlr_log(WLR_DEBUG, "Unable to allocate swaybg command");
			return;
		}
		snprintf(command, len + 1, "%s %d \"%s\" %s %s",
				config->swaybg_command, output_i, oc->background,
				oc->background_option,
				oc->background_fallback ? oc->background_fallback : "");
		wlr_log(WLR_DEBUG, "-> %s", command);

		char *const cmd[] = { "sh", "-c", command, NULL };
		output->bg_pid = fork();
		if (output->bg_pid == 0) {
			execvp(cmd[0], cmd);
		} else {
			free(command);
		}
	}

	if (oc) {
		switch (oc->dpms_state) {
		case DPMS_ON:
			wlr_log(WLR_DEBUG, "Turning on screen");
			wlr_output_enable(wlr_output, true);
			break;
		case DPMS_OFF:
			wlr_log(WLR_DEBUG, "Turning off screen");
			wlr_output_enable(wlr_output, false);
			break;
		case DPMS_IGNORE:
			break;
		}
	}
}

static struct output_config *get_output_config(char *name, char *identifier) {
	struct output_config *oc_name = NULL;
	int i = list_seq_find(config->output_configs, output_name_cmp, name);
	if (i >= 0) {
		oc_name = config->output_configs->items[i];
	}

	struct output_config *oc_id = NULL;
	i = list_seq_find(config->output_configs, output_name_cmp, identifier);
	if (i >= 0) {
		oc_id = config->output_configs->items[i];
	}

	struct output_config *result = NULL;
	if (oc_name && oc_id) {
		// Generate a config named `<identifier> on <name>` which contains a
		// merged copy of the identifier on name. This will make sure that both
		// identifier and name configs are respected, with identifier getting
		// priority
		size_t length = snprintf(NULL, 0, "%s on %s", identifier, name) + 1;
		char *temp = malloc(length);
		snprintf(temp, length, "%s on %s", identifier, name);

		result = new_output_config(temp);
		merge_output_config(result, oc_name);
		merge_output_config(result, oc_id);

		wlr_log(WLR_DEBUG, "Generated output config \"%s\" (enabled: %d)"
			" (%dx%d@%fHz position %d,%d scale %f transform %d) (bg %s %s)"
			" (dpms %d)", result->name, result->enabled, result->width,
			result->height, result->refresh_rate, result->x, result->y,
			result->scale, result->transform, result->background,
			result->background_option, result->dpms_state);

		free(temp);
	} else if (oc_name) {
		// No identifier config, just return a copy of the name config
		result = new_output_config(name);
		merge_output_config(result, oc_name);
	} else if (oc_id) {
		// No name config, just return a copy of the identifier config
		result = new_output_config(identifier);
		merge_output_config(result, oc_id);
	}

	return result;
}

void apply_output_config_to_outputs(struct output_config *oc) {
	// Try to find the output container and apply configuration now. If
	// this is during startup then there will be no container and config
	// will be applied during normal "new output" event from wlroots.
	bool wildcard = strcmp(oc->name, "*") == 0;
	char id[128];
	struct sway_output *sway_output;
	wl_list_for_each(sway_output, &root->all_outputs, link) {
		char *name = sway_output->wlr_output->name;
		output_get_identifier(id, sizeof(id), sway_output);
		if (wildcard || !strcmp(name, oc->name) || !strcmp(id, oc->name)) {
			struct output_config *current = new_output_config(oc->name);
			merge_output_config(current, oc);
			if (wildcard) {
				struct output_config *tmp = get_output_config(name, id);
				if (tmp) {
					free_output_config(current);
					current = tmp;
				}
			}
			apply_output_config(current, sway_output);
			free_output_config(current);

			if (!wildcard) {
				// Stop looking if the output config isn't applicable to all
				// outputs
				break;
			}
		}
	}
}

void free_output_config(struct output_config *oc) {
	if (!oc) {
		return;
	}
	free(oc->name);
	free(oc->background);
	free(oc->background_option);
	free(oc->background_fallback);
	free(oc);
}

static void default_output_config(struct output_config *oc,
		struct wlr_output *wlr_output) {
	oc->enabled = 1;
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode =
			wl_container_of(wlr_output->modes.prev, mode, link);
		oc->width = mode->width;
		oc->height = mode->height;
		oc->refresh_rate = mode->refresh;
	}
	oc->x = oc->y = -1;
	oc->scale = 1;
	oc->transform = WL_OUTPUT_TRANSFORM_NORMAL;
}

void create_default_output_configs(void) {
	struct sway_output *sway_output;
	wl_list_for_each(sway_output, &root->all_outputs, link) {
		char id[128];
		output_get_identifier(id, sizeof(id), sway_output);
		struct output_config *oc = new_output_config(id);
		default_output_config(oc, sway_output->wlr_output);
		list_add(config->output_configs, oc);
	}
}
