#ifndef _SWAY_INPUT_CURSOR_H
#define _SWAY_INPUT_CURSOR_H
#include <stdint.h>
#include "sway/input/seat.h"

#define SWAY_CURSOR_PRESSED_BUTTONS_CAP 32

struct sway_cursor {
	struct sway_seat *seat;
	struct wlr_cursor *cursor;
	struct {
		double x, y;
		struct sway_node *node;
	} previous;
	struct wlr_xcursor_manager *xcursor_manager;

	const char *image;
	struct wl_client *image_client;

	struct wl_listener motion;
	struct wl_listener motion_absolute;
	struct wl_listener button;
	struct wl_listener axis;

	struct wl_listener touch_down;
	struct wl_listener touch_up;
	struct wl_listener touch_motion;

	struct wl_listener tool_axis;
	struct wl_listener tool_tip;
	struct wl_listener tool_button;
	uint32_t tool_buttons;

	struct wl_listener request_set_cursor;

	// Mouse binding state
	uint32_t pressed_buttons[SWAY_CURSOR_PRESSED_BUTTONS_CAP];
	size_t pressed_button_count;
};

void sway_cursor_destroy(struct sway_cursor *cursor);
struct sway_cursor *sway_cursor_create(struct sway_seat *seat);

/**
 * "Rebase" a cursor on top of whatever view is underneath it.
 *
 * This chooses a cursor icon and sends a motion event to the surface.
 */
void cursor_rebase(struct sway_cursor *cursor);

/**
 * Like cursor_rebase, but also allows focus to change when the cursor enters a
 * new container.
 */
void cursor_send_pointer_motion(struct sway_cursor *cursor, uint32_t time_msec);

void dispatch_cursor_button(struct sway_cursor *cursor,
	struct wlr_input_device *device, uint32_t time_msec, uint32_t button,
	enum wlr_button_state state);

void cursor_set_image(struct sway_cursor *cursor, const char *image,
	struct wl_client *client);

void cursor_warp_to_container(struct sway_cursor *cursor,
	struct sway_container *container);

void cursor_warp_to_workspace(struct sway_cursor *cursor,
		struct sway_workspace *workspace);
#endif
