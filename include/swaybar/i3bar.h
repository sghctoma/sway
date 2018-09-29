#ifndef _SWAYBAR_I3BAR_H
#define _SWAYBAR_I3BAR_H

#include "bar.h"
#include "status_line.h"

struct i3bar_block {
	struct wl_list link;
	int ref_count;
	char *full_text, *short_text, *align;
	bool urgent;
	uint32_t *color;
	int min_width;
	char *name, *instance;
	bool separator;
	int separator_block_width;
	bool markup;
	// Airblader features
	uint32_t background;
	uint32_t border;
	int border_top;
	int border_bottom;
	int border_left;
	int border_right;
};

void i3bar_block_unref(struct i3bar_block *block);
bool i3bar_handle_readable(struct status_line *status);
enum hotspot_event_handling i3bar_block_send_click(struct status_line *status,
		struct i3bar_block *block, int x, int y, enum x11_button button);
enum x11_button wl_button_to_x11_button(uint32_t button);
enum x11_button wl_axis_to_x11_button(uint32_t axis, wl_fixed_t value);

#endif
