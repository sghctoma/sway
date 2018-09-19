#define _XOPEN_SOURCE 500
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wlr/util/log.h>
#ifdef __FreeBSD__
#include <dev/evdev/input-event-codes.h>
#else
#include <linux/input-event-codes.h>
#endif
#include "swaybar/render.h"
#include "swaybar/config.h"
#include "swaybar/event_loop.h"
#include "swaybar/status_line.h"
#include "swaybar/bar.h"
#include "swaybar/ipc.h"
#include "ipc-client.h"
#include "list.h"
#include "log.h"
#include "pool-buffer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

static void bar_init(struct swaybar *bar) {
	bar->config = init_config();
	wl_list_init(&bar->outputs);
}

void free_workspaces(struct wl_list *list) {
	struct swaybar_workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, list, link) {
		wl_list_remove(&ws->link);
		free(ws->name);
		free(ws);
	}
}

static void swaybar_output_free(struct swaybar_output *output) {
	if (!output) {
		return;
	}
	wlr_log(WLR_DEBUG, "Removing output %s", output->name);
	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wl_surface_destroy(output->surface);
	wl_output_destroy(output->output);
	destroy_buffer(&output->buffers[0]);
	destroy_buffer(&output->buffers[1]);
	free_workspaces(&output->workspaces);
	struct swaybar_hotspot *hotspot, *hotspot_tmp;
	wl_list_for_each_safe(hotspot, hotspot_tmp, &output->hotspots, link) {
		if (hotspot->destroy) {
			hotspot->destroy(hotspot->data);
		}
		free(hotspot);
	}
	wl_list_remove(&output->link);
	free(output->name);
	free(output);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct swaybar_output *output = data;
	output->width = width;
	output->height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_frame(output->bar, output);
}

static void layer_surface_closed(void *_output,
		struct zwlr_layer_surface_v1 *surface) {
	struct swaybar_output *output = _output;
	swaybar_output_free(output);
}

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaybar *bar = data;
	struct swaybar_pointer *pointer = &bar->pointer;
	struct swaybar_output *output;
	wl_list_for_each(output, &bar->outputs, link) {
		if (output->surface == surface) {
			pointer->current = output;
			break;
		}
	}
	int max_scale = 1;
	struct swaybar_output *_output;
	wl_list_for_each(_output, &bar->outputs, link) {
		if (_output->scale > max_scale) {
			max_scale = _output->scale;
		}
	}
	wl_surface_set_buffer_scale(pointer->cursor_surface, max_scale);
	wl_surface_attach(pointer->cursor_surface,
			wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
	wl_pointer_set_cursor(wl_pointer, serial, pointer->cursor_surface,
			pointer->cursor_image->hotspot_x / max_scale,
			pointer->cursor_image->hotspot_y / max_scale);
	wl_surface_commit(pointer->cursor_surface);
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct swaybar *bar = data;
	bar->pointer.current = NULL;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaybar *bar = data;
	bar->pointer.x = wl_fixed_to_int(surface_x);
	bar->pointer.y = wl_fixed_to_int(surface_y);
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct swaybar *bar = data;
	struct swaybar_pointer *pointer = &bar->pointer;
	struct swaybar_output *output = pointer->current;
	if (!sway_assert(output, "button with no active output")) {
		return;
	}
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}
	struct swaybar_hotspot *hotspot;
	wl_list_for_each(hotspot, &output->hotspots, link) {
		double x = pointer->x * output->scale;
		double y = pointer->y * output->scale;
		if (x >= hotspot->x
				&& y >= hotspot->y
				&& x < hotspot->x + hotspot->width
				&& y < hotspot->y + hotspot->height) {
			if (HOTSPOT_IGNORE == hotspot->callback(output, pointer->x, pointer->y,
					wl_button_to_x11_button(button), hotspot->data)) {
				return;
			}
		}
	}
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	struct swaybar *bar = data;
	struct swaybar_pointer *pointer = &bar->pointer;
	struct swaybar_output *output = bar->pointer.current;
	if (!sway_assert(output, "axis with no active output")) {
		return;
	}

	struct swaybar_hotspot *hotspot;
	wl_list_for_each(hotspot, &output->hotspots, link) {
		double x = pointer->x * output->scale;
		double y = pointer->y * output->scale;
		if (x >= hotspot->x
				&& y >= hotspot->y
				&& x < hotspot->x + hotspot->width
				&& y < hotspot->y + hotspot->height) {
			if (HOTSPOT_IGNORE == hotspot->callback(
					output, pointer->x, pointer->y,
					wl_axis_to_x11_button(axis, value), hotspot->data)) {
				return;
			}
		}
	}

	double amt = wl_fixed_to_double(value);
	if (amt == 0.0) {
		return;
	}

	// last doesn't actually need initialization,
	// but gcc (7.3.1) is too dumb to figure it out
	struct swaybar_workspace *first = NULL;
	struct swaybar_workspace *active = NULL;
	struct swaybar_workspace *last = NULL;

	struct swaybar_workspace *iter;
	wl_list_for_each(iter, &output->workspaces, link) {
		if (!first) {
			first = iter;
		}

		if (iter->visible) {
			active = iter;
		}

		last = iter;
	}

	if (!sway_assert(active, "axis with null workspace")) {
		return;
	}

	struct swaybar_workspace *new;

	if (amt > 0.0) {
		if (active == first) {
			if (!bar->config->wrap_scroll) {
				return;
			}
			new = last;
		}

		new = wl_container_of(active->link.prev, new, link);
	} else {
		if (active == last) {
			if (!bar->config->wrap_scroll) {
				return;
			}
			new = first;
		}

		new = wl_container_of(active->link.next, new, link);
	}

	ipc_send_workspace_command(bar, new->name);
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
	// Who cares
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis_source) {
	// Who cares
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis) {
	// Who cares
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t discrete) {
	// Who cares
}

struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	struct swaybar *bar = data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
		bar->pointer.pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(bar->pointer.pointer, &pointer_listener, bar);
	}
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
		const char *name) {
	// Who cares
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void output_done(void *data, struct wl_output *output) {
	// Who cares
}

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t factor) {
	struct swaybar_output *output = data;
	output->scale = factor;
	if (output->surface) {
		render_frame(output->bar, output);
	}
}

struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void xdg_output_handle_logical_position(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
	// Who cares
}

static void xdg_output_handle_logical_size(void *data,
		struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
	// Who cares
}

static void xdg_output_handle_done(void *data,
		struct zxdg_output_v1 *xdg_output) {
	// Who cares
}

static void xdg_output_handle_name(void *data,
		struct zxdg_output_v1 *xdg_output, const char *name) {
	struct swaybar_output *output = data;
	free(output->name);
	output->name = strdup(name);
}

static void xdg_output_handle_description(void *data,
		struct zxdg_output_v1 *xdg_output, const char *description) {
	// Who cares
}

struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.done = xdg_output_handle_done,
	.name = xdg_output_handle_name,
	.description = xdg_output_handle_description,
};

static bool bar_uses_output(struct swaybar *bar, const char *name) {
	if (bar->config->all_outputs) {
		return true;
	}
	struct config_output *coutput;
	wl_list_for_each(coutput, &bar->config->outputs, link) {
		if (strcmp(coutput->name, name) == 0) {
			return true;
		}
	}
	return false;
}

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaybar *bar = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		bar->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 3);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		bar->seat = wl_registry_bind(registry, name,
				&wl_seat_interface, 1);
		wl_seat_add_listener(bar->seat, &seat_listener, bar);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		bar->shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaybar_output *output =
			calloc(1, sizeof(struct swaybar_output));
		output->bar = bar;
		output->output = wl_registry_bind(registry, name,
				&wl_output_interface, 3);
		wl_output_add_listener(output->output, &output_listener, output);
		output->scale = 1;
		output->wl_name = name;
		wl_list_init(&output->workspaces);
		wl_list_init(&output->hotspots);
		wl_list_insert(&bar->outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		bar->layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		bar->xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, 2);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct swaybar *bar = data;
	struct swaybar_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &bar->outputs, link) {
		if (output->wl_name == name) {
			swaybar_output_free(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void render_all_frames(struct swaybar *bar) {
	struct swaybar_output *output;
	wl_list_for_each(output, &bar->outputs, link) {
		render_frame(bar, output);
	}
}

void bar_setup(struct swaybar *bar,
		const char *socket_path, const char *bar_id) {
	bar_init(bar);
	init_event_loop();

	bar->ipc_socketfd = ipc_open_socket(socket_path);
	bar->ipc_event_socketfd = ipc_open_socket(socket_path);
	ipc_initialize(bar, bar_id);
	if (bar->config->status_command) {
		bar->status = status_line_init(bar->config->status_command);
	}

	bar->display = wl_display_connect(NULL);
	assert(bar->display);

	struct wl_registry *registry = wl_display_get_registry(bar->display);
	wl_registry_add_listener(registry, &registry_listener, bar);
	wl_display_roundtrip(bar->display);
	assert(bar->compositor && bar->layer_shell && bar->shm &&
		bar->xdg_output_manager);

	struct swaybar_output *output;
	wl_list_for_each(output, &bar->outputs, link) {
		output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
			bar->xdg_output_manager, output->output);
		zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener,
			output);
	}
	wl_display_roundtrip(bar->display);

	struct swaybar_output *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &bar->outputs, link) {
		if (!bar_uses_output(bar, output->name)) {
			zxdg_output_v1_destroy(output->xdg_output);
			wl_output_destroy(output->output);
			wl_list_remove(&output->link);
			free(output);
		}
	}

	struct swaybar_pointer *pointer = &bar->pointer;

	int max_scale = 1;
	wl_list_for_each(output, &bar->outputs, link) {
		if (output->scale > max_scale) {
			max_scale = output->scale;
		}
	}

	pointer->cursor_theme =
		wl_cursor_theme_load(NULL, 24 * max_scale, bar->shm);
	assert(pointer->cursor_theme);
	struct wl_cursor *cursor;
	cursor = wl_cursor_theme_get_cursor(pointer->cursor_theme, "left_ptr");
	assert(cursor);
	pointer->cursor_image = cursor->images[0];
	pointer->cursor_surface = wl_compositor_create_surface(bar->compositor);
	assert(pointer->cursor_surface);

	wl_list_for_each(output, &bar->outputs, link) {
		output->surface = wl_compositor_create_surface(bar->compositor);
		assert(output->surface);
		output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
				bar->layer_shell, output->surface, output->output,
				ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "panel");
		assert(output->layer_surface);
		zwlr_layer_surface_v1_add_listener(output->layer_surface,
				&layer_surface_listener, output);
		zwlr_layer_surface_v1_set_anchor(output->layer_surface,
				bar->config->position);
	}
	ipc_get_workspaces(bar);
	render_all_frames(bar);
}

static void display_in(int fd, short mask, void *data) {
	struct swaybar *bar = data;
	if (wl_display_dispatch(bar->display) == -1) {
		bar_teardown(bar);
		exit(0);
	}
}

static void ipc_in(int fd, short mask, void *data) {
	struct swaybar *bar = data;
	if (handle_ipc_readable(bar)) {
		render_all_frames(bar);
	}
}

static void status_in(int fd, short mask, void *data) {
	struct swaybar *bar = data;
	if (mask & (POLLHUP | POLLERR)) {
		status_error(bar->status, "[error reading from status command]");
		render_all_frames(bar);
		remove_event(fd);
	} else if (status_handle_readable(bar->status)) {
		render_all_frames(bar);
	}
}

void bar_run(struct swaybar *bar) {
	add_event(wl_display_get_fd(bar->display), POLLIN, display_in, bar);
	add_event(bar->ipc_event_socketfd, POLLIN, ipc_in, bar);
	if (bar->status) {
		add_event(bar->status->read_fd, POLLIN, status_in, bar);
	}
	while (1) {
		event_loop_poll();
	}
}

static void free_outputs(struct wl_list *list) {
	struct swaybar_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, list, link) {
		swaybar_output_free(output);
	}
}

void bar_teardown(struct swaybar *bar) {
	free_outputs(&bar->outputs);
	if (bar->config) {
		free_config(bar->config);
	}
	close(bar->ipc_event_socketfd);
	close(bar->ipc_socketfd);
	if (bar->status) {
		status_line_free(bar->status);
	}
}
