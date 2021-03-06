#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell_v6.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "cairo.h"
#include "pango.h"
#include "sway/config.h"
#include "sway/desktop.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "log.h"
#include "stringop.h"

const char *container_type_to_str(enum sway_container_type type) {
	switch (type) {
	case C_ROOT:
		return "C_ROOT";
	case C_OUTPUT:
		return "C_OUTPUT";
	case C_WORKSPACE:
		return "C_WORKSPACE";
	case C_CONTAINER:
		return "C_CONTAINER";
	case C_VIEW:
		return "C_VIEW";
	default:
		return "C_UNKNOWN";
	}
}

void container_create_notify(struct sway_container *container) {
	if (container->type == C_VIEW) {
		ipc_event_window(container, "new");
	} else if (container->type == C_WORKSPACE) {
		ipc_event_workspace(NULL, container, "init");
	}
	wl_signal_emit(&root_container.sway_root->events.new_container, container);
}

void container_update_textures_recursive(struct sway_container *con) {
	if (con->type == C_CONTAINER || con->type == C_VIEW) {
		container_update_title_textures(con);
	}

	if (con->type == C_VIEW) {
		view_update_marks_textures(con->sway_view);
	} else {
		for (int i = 0; i < con->children->length; ++i) {
			struct sway_container *child = con->children->items[i];
			container_update_textures_recursive(child);
		}

		if (con->type == C_WORKSPACE) {
			for (int i = 0; i < con->sway_workspace->floating->length; ++i) {
				struct sway_container *floater =
					con->sway_workspace->floating->items[i];
				container_update_textures_recursive(floater);
			}
		}
	}
}

struct sway_container *container_create(enum sway_container_type type) {
	// next id starts at 1 because 0 is assigned to root_container in layout.c
	static size_t next_id = 1;
	struct sway_container *c = calloc(1, sizeof(struct sway_container));
	if (!c) {
		return NULL;
	}
	c->id = next_id++;
	c->layout = L_NONE;
	c->type = type;
	c->alpha = 1.0f;

	if (type != C_VIEW) {
		c->children = create_list();
		c->current.children = create_list();
	}
	c->outputs = create_list();

	wl_signal_init(&c->events.destroy);

	c->has_gaps = false;
	c->gaps_inner = 0;
	c->gaps_outer = 0;
	c->current_gaps = 0;

	return c;
}

void container_destroy(struct sway_container *con) {
	if (!sway_assert(con->type == C_CONTAINER || con->type == C_VIEW,
				"Expected a container or view")) {
		return;
	}
	if (!sway_assert(con->destroying,
				"Tried to free container which wasn't marked as destroying")) {
		return;
	}
	if (!sway_assert(con->ntxnrefs == 0, "Tried to free container "
				"which is still referenced by transactions")) {
		return;
	}
	free(con->name);
	free(con->formatted_title);
	wlr_texture_destroy(con->title_focused);
	wlr_texture_destroy(con->title_focused_inactive);
	wlr_texture_destroy(con->title_unfocused);
	wlr_texture_destroy(con->title_urgent);
	list_free(con->children);
	list_free(con->current.children);
	list_free(con->outputs);

	if (con->type == C_VIEW) {
		struct sway_view *view = con->sway_view;
		view->swayc = NULL;
		free(view->title_format);
		view->title_format = NULL;

		if (view->destroying) {
			view_destroy(view);
		}
	}

	free(con);
}

void container_begin_destroy(struct sway_container *con) {
	if (!sway_assert(con->type == C_CONTAINER || con->type == C_VIEW,
				"Expected a container or view")) {
		return;
	}

	if (con->type == C_VIEW) {
		ipc_event_window(con, "close");
	}
	wl_signal_emit(&con->events.destroy, con);

	container_end_mouse_operation(con);

	con->destroying = true;
	container_set_dirty(con);

	if (con->scratchpad) {
		root_scratchpad_remove_container(con);
	}

	if (con->parent) {
		container_remove_child(con);
	}
}

struct sway_container *container_reap_empty(struct sway_container *con) {
	while (con && con->type == C_CONTAINER) {
		struct sway_container *next = con->parent;
		if (con->children->length == 0) {
			container_begin_destroy(con);
		}
		con = next;
	}
	if (con && con->type == C_WORKSPACE) {
		workspace_consider_destroy(con);
		if (con->destroying) {
			con = con->parent;
		}
	}
	return con;
}

struct sway_container *container_flatten(struct sway_container *container) {
	while (container->type == C_CONTAINER && container->children->length == 1) {
		struct sway_container *child = container->children->items[0];
		struct sway_container *parent = container->parent;
		container_replace_child(container, child);
		container_begin_destroy(container);
		container = parent;
	}
	return container;
}

static void container_close_func(struct sway_container *container, void *data) {
	if (container->type == C_VIEW) {
		view_close(container->sway_view);
	}
}

struct sway_container *container_close(struct sway_container *con) {
	if (!sway_assert(con != NULL,
			"container_close called with a NULL container")) {
		return NULL;
	}

	struct sway_container *parent = con->parent;

	if (con->type == C_VIEW) {
		view_close(con->sway_view);
	} else if (con->type == C_CONTAINER) {
		container_for_each_child(con, container_close_func, NULL);
	} else if (con->type == C_WORKSPACE) {
		workspace_for_each_container(con, container_close_func, NULL);
	}

	return parent;
}

struct sway_container *container_view_create(struct sway_container *sibling,
		struct sway_view *sway_view) {
	if (!sway_assert(sibling,
			"container_view_create called with NULL sibling/parent")) {
		return NULL;
	}
	const char *title = view_get_title(sway_view);
	struct sway_container *swayc = container_create(C_VIEW);
	wlr_log(WLR_DEBUG, "Adding new view %p:%s to container %p %d %s",
		swayc, title, sibling, sibling ? sibling->type : 0, sibling->name);
	// Setup values
	swayc->sway_view = sway_view;
	swayc->width = 0;
	swayc->height = 0;

	if (sibling->type == C_WORKSPACE) {
		// Case of focused workspace, just create as child of it
		container_add_child(sibling, swayc);
	} else {
		// Regular case, create as sibling of current container
		container_add_sibling(sibling, swayc);
	}
	container_create_notify(swayc);
	return swayc;
}

struct sway_container *container_find_child(struct sway_container *container,
		bool (*test)(struct sway_container *view, void *data), void *data) {
	if (!sway_assert(container->type == C_CONTAINER ||
				container->type == C_VIEW, "Expected a container or view")) {
		return NULL;
	}
	if (!container->children) {
		return NULL;
	}
	for (int i = 0; i < container->children->length; ++i) {
		struct sway_container *child = container->children->items[i];
		if (test(child, data)) {
			return child;
		}
		struct sway_container *res = container_find_child(child, test, data);
		if (res) {
			return res;
		}
	}
	return NULL;
}

struct sway_container *container_parent(struct sway_container *container,
		enum sway_container_type type) {
	if (!sway_assert(container, "container is NULL")) {
		return NULL;
	}
	if (!sway_assert(type < C_TYPES && type >= C_ROOT, "invalid type")) {
		return NULL;
	}
	do {
		container = container->parent;
	} while (container && container->type != type);
	return container;
}

static void surface_at_view(struct sway_container *swayc, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (!sway_assert(swayc->type == C_VIEW, "Expected a view")) {
		return;
	}
	struct sway_view *sview = swayc->sway_view;
	double view_sx = lx - sview->x + sview->geometry.x;
	double view_sy = ly - sview->y + sview->geometry.y;

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;
	switch (sview->type) {
#ifdef HAVE_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		_surface = wlr_surface_surface_at(sview->surface,
				view_sx, view_sy, &_sx, &_sy);
		break;
#endif
	case SWAY_VIEW_XDG_SHELL_V6:
		_surface = wlr_xdg_surface_v6_surface_at(
				sview->wlr_xdg_surface_v6,
				view_sx, view_sy, &_sx, &_sy);
		break;
	case SWAY_VIEW_XDG_SHELL:
		_surface = wlr_xdg_surface_surface_at(
				sview->wlr_xdg_surface,
				view_sx, view_sy, &_sx, &_sy);
		break;
	}
	if (_surface) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
	}
}

/**
 * container_at for a container with layout L_TABBED.
 */
static struct sway_container *container_at_tabbed(struct sway_container *parent,
		double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (ly < parent->y || ly > parent->y + parent->height) {
		return NULL;
	}
	struct sway_seat *seat = input_manager_current_seat(input_manager);

	// Tab titles
	int title_height = container_titlebar_height();
	if (ly < parent->y + title_height) {
		int tab_width = parent->width / parent->children->length;
		int child_index = (lx - parent->x) / tab_width;
		if (child_index >= parent->children->length) {
			child_index = parent->children->length - 1;
		}
		struct sway_container *child = parent->children->items[child_index];
		return seat_get_focus_inactive(seat, child);
	}

	// Surfaces
	struct sway_container *current = seat_get_active_child(seat, parent);

	return tiling_container_at(current, lx, ly, surface, sx, sy);
}

/**
 * container_at for a container with layout L_STACKED.
 */
static struct sway_container *container_at_stacked(
		struct sway_container *parent, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (ly < parent->y || ly > parent->y + parent->height) {
		return NULL;
	}
	struct sway_seat *seat = input_manager_current_seat(input_manager);

	// Title bars
	int title_height = container_titlebar_height();
	int child_index = (ly - parent->y) / title_height;
	if (child_index < parent->children->length) {
		struct sway_container *child = parent->children->items[child_index];
		return seat_get_focus_inactive(seat, child);
	}

	// Surfaces
	struct sway_container *current = seat_get_active_child(seat, parent);

	return tiling_container_at(current, lx, ly, surface, sx, sy);
}

/**
 * container_at for a container with layout L_HORIZ or L_VERT.
 */
static struct sway_container *container_at_linear(struct sway_container *parent,
		double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	for (int i = 0; i < parent->children->length; ++i) {
		struct sway_container *child = parent->children->items[i];
		struct wlr_box box = {
			.x = child->x,
			.y = child->y,
			.width = child->width,
			.height = child->height,
		};
		if (wlr_box_contains_point(&box, lx, ly)) {
			return tiling_container_at(child, lx, ly, surface, sx, sy);
		}
	}
	return NULL;
}

static struct sway_container *floating_container_at(double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	for (int i = 0; i < root_container.children->length; ++i) {
		struct sway_container *output = root_container.children->items[i];
		for (int j = 0; j < output->children->length; ++j) {
			struct sway_container *workspace = output->children->items[j];
			struct sway_workspace *ws = workspace->sway_workspace;
			if (!workspace_is_visible(workspace)) {
				continue;
			}
			// Items at the end of the list are on top, so iterate the list in
			// reverse.
			for (int k = ws->floating->length - 1; k >= 0; --k) {
				struct sway_container *floater = ws->floating->items[k];
				struct wlr_box box = {
					.x = floater->x,
					.y = floater->y,
					.width = floater->width,
					.height = floater->height,
				};
				if (wlr_box_contains_point(&box, lx, ly)) {
					return tiling_container_at(floater, lx, ly,
							surface, sx, sy);
				}
			}
		}
	}
	return NULL;
}

struct sway_container *tiling_container_at(
		struct sway_container *con, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (con->type == C_VIEW) {
		surface_at_view(con, lx, ly, surface, sx, sy);
		return con;
	}
	if (!con->children->length) {
		return NULL;
	}

	switch (con->layout) {
	case L_HORIZ:
	case L_VERT:
		return container_at_linear(con, lx, ly, surface, sx, sy);
	case L_TABBED:
		return container_at_tabbed(con, lx, ly, surface, sx, sy);
	case L_STACKED:
		return container_at_stacked(con, lx, ly, surface, sx, sy);
	case L_NONE:
		return NULL;
	}
	return NULL;
}

static bool surface_is_popup(struct wlr_surface *surface) {
	if (wlr_surface_is_xdg_surface(surface)) {
		struct wlr_xdg_surface *xdg_surface =
			wlr_xdg_surface_from_wlr_surface(surface);
		while (xdg_surface) {
			if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
				return true;
			}
			xdg_surface = xdg_surface->toplevel->parent;
		}
		return false;
	}

	if (wlr_surface_is_xdg_surface_v6(surface)) {
		struct wlr_xdg_surface_v6 *xdg_surface_v6 =
			wlr_xdg_surface_v6_from_wlr_surface(surface);
		while (xdg_surface_v6) {
			if (xdg_surface_v6->role == WLR_XDG_SURFACE_V6_ROLE_POPUP) {
				return true;
			}
			xdg_surface_v6 = xdg_surface_v6->toplevel->parent;
		}
		return false;
	}

	return false;
}

struct sway_container *container_at(struct sway_container *workspace,
		double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (!sway_assert(workspace->type == C_WORKSPACE, "Expected a workspace")) {
		return NULL;
	}
	struct sway_container *c;
	struct sway_seat *seat = input_manager_current_seat(input_manager);
	struct sway_container *focus =
		seat_get_focus_inactive(seat, &root_container);
	bool is_floating = focus && container_is_floating_or_child(focus);
	// Focused view's popups
	if (focus && focus->type == C_VIEW) {
		surface_at_view(focus, lx, ly, surface, sx, sy);
		if (*surface && surface_is_popup(*surface)) {
			return focus;
		}
		*surface = NULL;
	}
	// If focused is floating, focused view's non-popups
	if (focus && focus->type == C_VIEW && is_floating) {
		surface_at_view(focus, lx, ly, surface, sx, sy);
		if (*surface) {
			return focus;
		}
		*surface = NULL;
	}
	// Floating (non-focused)
	if ((c = floating_container_at(lx, ly, surface, sx, sy))) {
		return c;
	}
	// If focused is tiling, focused view's non-popups
	if (focus && focus->type == C_VIEW && !is_floating) {
		surface_at_view(focus, lx, ly, surface, sx, sy);
		if (*surface) {
			return focus;
		}
		*surface = NULL;
	}
	// Tiling (non-focused)
	if ((c = tiling_container_at(workspace, lx, ly, surface, sx, sy))) {
		return c;
	}
	return NULL;
}

void container_for_each_child(struct sway_container *container,
		void (*f)(struct sway_container *container, void *data),
		void *data) {
	if (!sway_assert(container->type == C_CONTAINER ||
				container->type == C_VIEW, "Expected a container or view")) {
		return;
	}
	if (container->children)  {
		for (int i = 0; i < container->children->length; ++i) {
			struct sway_container *child = container->children->items[i];
			f(child, data);
			container_for_each_child(child, f, data);
		}
	}
}

bool container_has_ancestor(struct sway_container *descendant,
		struct sway_container *ancestor) {
	while (descendant && descendant->type != C_ROOT) {
		descendant = descendant->parent;
		if (descendant == ancestor) {
			return true;
		}
	}
	return false;
}

int container_count_descendants_of_type(struct sway_container *con,
		enum sway_container_type type) {
	int children = 0;
	if (con->type == type) {
		children++;
	}
	if (con->children) {
		for (int i = 0; i < con->children->length; i++) {
			struct sway_container *child = con->children->items[i];
			children += container_count_descendants_of_type(child, type);
		}
	}
	return children;
}

void container_damage_whole(struct sway_container *container) {
	for (int i = 0; i < root_container.children->length; ++i) {
		struct sway_container *cont = root_container.children->items[i];
		if (cont->type == C_OUTPUT) {
			output_damage_whole_container(cont->sway_output, container);
		}
	}
}

/**
 * Return the output which will be used for scale purposes.
 * This is the most recently entered output.
 */
struct sway_output *container_get_effective_output(struct sway_container *con) {
	if (con->outputs->length == 0) {
		return NULL;
	}
	return con->outputs->items[con->outputs->length - 1];
}

static void update_title_texture(struct sway_container *con,
		struct wlr_texture **texture, struct border_colors *class) {
	if (!sway_assert(con->type == C_CONTAINER || con->type == C_VIEW,
			"Unexpected type %s", container_type_to_str(con->type))) {
		return;
	}
	struct sway_output *output = container_get_effective_output(con);
	if (!output) {
		return;
	}
	if (*texture) {
		wlr_texture_destroy(*texture);
		*texture = NULL;
	}
	if (!con->formatted_title) {
		return;
	}

	double scale = output->wlr_output->scale;
	int width = 0;
	int height = con->title_height * scale;

	cairo_t *c = cairo_create(NULL);
	get_text_size(c, config->font, &width, NULL, scale, config->pango_markup,
			"%s", con->formatted_title);
	cairo_destroy(c);

	cairo_surface_t *surface = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cairo = cairo_create(surface);
	cairo_set_source_rgba(cairo, class->background[0], class->background[1],
			class->background[2], class->background[3]);
	cairo_paint(cairo);
	PangoContext *pango = pango_cairo_create_context(cairo);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_set_source_rgba(cairo, class->text[0], class->text[1],
			class->text[2], class->text[3]);
	cairo_move_to(cairo, 0, 0);

	pango_printf(cairo, config->font, scale, config->pango_markup,
			"%s", con->formatted_title);

	cairo_surface_flush(surface);
	unsigned char *data = cairo_image_surface_get_data(surface);
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	struct wlr_renderer *renderer = wlr_backend_get_renderer(
			output->wlr_output->backend);
	*texture = wlr_texture_from_pixels(
			renderer, WL_SHM_FORMAT_ARGB8888, stride, width, height, data);
	cairo_surface_destroy(surface);
	g_object_unref(pango);
	cairo_destroy(cairo);
}

void container_update_title_textures(struct sway_container *container) {
	update_title_texture(container, &container->title_focused,
			&config->border_colors.focused);
	update_title_texture(container, &container->title_focused_inactive,
			&config->border_colors.focused_inactive);
	update_title_texture(container, &container->title_unfocused,
			&config->border_colors.unfocused);
	update_title_texture(container, &container->title_urgent,
			&config->border_colors.urgent);
	container_damage_whole(container);
}

void container_calculate_title_height(struct sway_container *container) {
	if (!container->formatted_title) {
		container->title_height = 0;
		return;
	}
	cairo_t *cairo = cairo_create(NULL);
	int height;
	get_text_size(cairo, config->font, NULL, &height, 1, config->pango_markup,
			"%s", container->formatted_title);
	cairo_destroy(cairo);
	container->title_height = height;
}

/**
 * Calculate and return the length of the tree representation.
 * An example tree representation is: V[Terminal, Firefox]
 * If buffer is not NULL, also populate the buffer with the representation.
 */
static size_t get_tree_representation(struct sway_container *parent, char *buffer) {
	size_t len = 2;
	switch (parent->layout) {
	case L_VERT:
		lenient_strcat(buffer, "V[");
		break;
	case L_HORIZ:
		lenient_strcat(buffer, "H[");
		break;
	case L_TABBED:
		lenient_strcat(buffer, "T[");
		break;
	case L_STACKED:
		lenient_strcat(buffer, "S[");
		break;
	case L_NONE:
		lenient_strcat(buffer, "D[");
		break;
	}
	for (int i = 0; i < parent->children->length; ++i) {
		if (i != 0) {
			++len;
			lenient_strcat(buffer, " ");
		}
		struct sway_container *child = parent->children->items[i];
		const char *identifier = NULL;
		if (child->type == C_VIEW) {
			identifier = view_get_class(child->sway_view);
			if (!identifier) {
				identifier = view_get_app_id(child->sway_view);
			}
		} else {
			identifier = child->formatted_title;
		}
		if (identifier) {
			len += strlen(identifier);
			lenient_strcat(buffer, identifier);
		} else {
			len += 6;
			lenient_strcat(buffer, "(null)");
		}
	}
	++len;
	lenient_strcat(buffer, "]");
	return len;
}

void container_notify_subtree_changed(struct sway_container *container) {
	if (!container || container->type < C_WORKSPACE) {
		return;
	}
	free(container->formatted_title);
	container->formatted_title = NULL;

	size_t len = get_tree_representation(container, NULL);
	char *buffer = calloc(len + 1, sizeof(char));
	if (!sway_assert(buffer, "Unable to allocate title string")) {
		return;
	}
	get_tree_representation(container, buffer);

	container->formatted_title = buffer;
	if (container->type != C_WORKSPACE) {
		container_calculate_title_height(container);
		container_update_title_textures(container);
		container_notify_subtree_changed(container->parent);
	}
}

size_t container_titlebar_height() {
	return config->font_height + TITLEBAR_V_PADDING * 2;
}

void container_init_floating(struct sway_container *con) {
	if (!sway_assert(con->type == C_VIEW || con->type == C_CONTAINER,
			"Expected a view or container")) {
		return;
	}
	struct sway_container *ws = container_parent(con, C_WORKSPACE);
	int min_width, min_height;
	int max_width, max_height;

	if (config->floating_minimum_width == -1) { // no minimum
		min_width = 0;
	} else if (config->floating_minimum_width == 0) { // automatic
		min_width = 75;
	} else {
		min_width = config->floating_minimum_width;
	}

	if (config->floating_minimum_height == -1) { // no minimum
		min_height = 0;
	} else if (config->floating_minimum_height == 0) { // automatic
		min_height = 50;
	} else {
		min_height = config->floating_minimum_height;
	}

	if (config->floating_maximum_width == -1) { // no maximum
		max_width = INT_MAX;
	} else if (config->floating_maximum_width == 0) { // automatic
		max_width = ws->width * 0.6666;
	} else {
		max_width = config->floating_maximum_width;
	}

	if (config->floating_maximum_height == -1) { // no maximum
		max_height = INT_MAX;
	} else if (config->floating_maximum_height == 0) { // automatic
		max_height = ws->height * 0.6666;
	} else {
		max_height = config->floating_maximum_height;
	}

	if (con->type == C_CONTAINER) {
		con->width = max_width;
		con->height = max_height;
		con->x = ws->x + (ws->width - con->width) / 2;
		con->y = ws->y + (ws->height - con->height) / 2;
	} else {
		struct sway_view *view = con->sway_view;
		view->width = fmax(min_width, fmin(view->natural_width, max_width));
		view->height = fmax(min_height, fmin(view->natural_height, max_height));
		view->x = ws->x + (ws->width - view->width) / 2;
		view->y = ws->y + (ws->height - view->height) / 2;

		// If the view's border is B_NONE then these properties are ignored.
		view->border_top = view->border_bottom = true;
		view->border_left = view->border_right = true;

		container_set_geometry_from_floating_view(view->swayc);
	}
}

void container_set_floating(struct sway_container *container, bool enable) {
	if (container_is_floating(container) == enable) {
		return;
	}

	struct sway_seat *seat = input_manager_current_seat(input_manager);
	struct sway_container *workspace = container_parent(container, C_WORKSPACE);

	if (enable) {
		struct sway_container *old_parent = container_remove_child(container);
		workspace_add_floating(workspace, container);
		container_init_floating(container);
		if (container->type == C_VIEW) {
			view_set_tiled(container->sway_view, false);
		}
		container_reap_empty(old_parent);
	} else {
		// Returning to tiled
		if (container->scratchpad) {
			root_scratchpad_remove_container(container);
		}
		container_remove_child(container);
		struct sway_container *reference =
			seat_get_focus_inactive_tiling(seat, workspace);
		if (reference->type == C_VIEW) {
			reference = reference->parent;
		}
		container_add_child(reference, container);
		container->width = container->parent->width;
		container->height = container->parent->height;
		if (container->type == C_VIEW) {
			view_set_tiled(container->sway_view, true);
		}
		container->is_sticky = false;
	}

	container_end_mouse_operation(container);

	ipc_event_window(container, "floating");
}

void container_set_geometry_from_floating_view(struct sway_container *con) {
	if (!sway_assert(con->type == C_VIEW, "Expected a view")) {
		return;
	}
	if (!sway_assert(container_is_floating(con),
				"Expected a floating view")) {
		return;
	}
	struct sway_view *view = con->sway_view;
	size_t border_width = 0;
	size_t top = 0;

	if (!view->using_csd) {
		border_width = view->border_thickness * (view->border != B_NONE);
		top = view->border == B_NORMAL ?
			container_titlebar_height() : border_width;
	}

	con->x = view->x - border_width;
	con->y = view->y - top;
	con->width = view->width + border_width * 2;
	con->height = top + view->height + border_width;
	container_set_dirty(con);
}

bool container_is_floating(struct sway_container *container) {
	return container->parent && container->parent->type == C_WORKSPACE &&
		list_find(container->parent->sway_workspace->floating, container) != -1;
}

void container_get_box(struct sway_container *container, struct wlr_box *box) {
	box->x = container->x;
	box->y = container->y;
	box->width = container->width;
	box->height = container->height;
}

/**
 * Translate the container's position as well as all children.
 */
void container_floating_translate(struct sway_container *con,
		double x_amount, double y_amount) {
	con->x += x_amount;
	con->y += y_amount;
	if (con->type == C_VIEW) {
		con->sway_view->x += x_amount;
		con->sway_view->y += y_amount;
	} else {
		for (int i = 0; i < con->children->length; ++i) {
			struct sway_container *child = con->children->items[i];
			container_floating_translate(child, x_amount, y_amount);
		}
	}
	container_set_dirty(con);
}

/**
 * Choose an output for the floating container's new position.
 *
 * If the center of the container intersects an output then we'll choose that
 * one, otherwise we'll choose whichever output is closest to the container's
 * center.
 */
struct sway_container *container_floating_find_output(
		struct sway_container *con) {
	double center_x = con->x + con->width / 2;
	double center_y = con->y + con->height / 2;
	struct sway_container *closest_output = NULL;
	double closest_distance = DBL_MAX;
	for (int i = 0; i < root_container.children->length; ++i) {
		struct sway_container *output = root_container.children->items[i];
		struct wlr_box output_box;
		double closest_x, closest_y;
		container_get_box(output, &output_box);
		wlr_box_closest_point(&output_box, center_x, center_y,
				&closest_x, &closest_y);
		if (center_x == closest_x && center_y == closest_y) {
			// The center of the floating container is on this output
			return output;
		}
		double x_dist = closest_x - center_x;
		double y_dist = closest_y - center_y;
		double distance = x_dist * x_dist + y_dist * y_dist;
		if (distance < closest_distance) {
			closest_output = output;
			closest_distance = distance;
		}
	}
	return closest_output;
}

void container_floating_move_to(struct sway_container *con,
		double lx, double ly) {
	if (!sway_assert(container_is_floating(con),
			"Expected a floating container")) {
		return;
	}
	container_floating_translate(con, lx - con->x, ly - con->y);
	struct sway_container *old_workspace = container_parent(con, C_WORKSPACE);
	struct sway_container *new_output = container_floating_find_output(con);
	if (!sway_assert(new_output, "Unable to find any output")) {
		return;
	}
	struct sway_container *new_workspace =
		output_get_active_workspace(new_output->sway_output);
	if (old_workspace != new_workspace) {
		container_remove_child(con);
		workspace_add_floating(new_workspace, con);
		arrange_windows(old_workspace);
		arrange_windows(new_workspace);
		workspace_detect_urgent(old_workspace);
		workspace_detect_urgent(new_workspace);
	}
}

void container_floating_move_to_center(struct sway_container *con) {
	if (!sway_assert(container_is_floating(con),
			"Expected a floating container")) {
		return;
	}
	struct sway_container *ws = container_parent(con, C_WORKSPACE);
	double new_lx = ws->x + (ws->width - con->width) / 2;
	double new_ly = ws->y + (ws->height - con->height) / 2;
	container_floating_translate(con, new_lx - con->x, new_ly - con->y);
}

void container_set_dirty(struct sway_container *container) {
	if (container->dirty) {
		return;
	}
	container->dirty = true;
	list_add(server.dirty_containers, container);
}

static bool find_urgent_iterator(struct sway_container *con, void *data) {
	return con->type == C_VIEW && view_is_urgent(con->sway_view);
}

bool container_has_urgent_child(struct sway_container *container) {
	return container_find_child(container, find_urgent_iterator, NULL);
}

void container_end_mouse_operation(struct sway_container *container) {
	struct sway_seat *seat;
	wl_list_for_each(seat, &input_manager->seats, link) {
		if (seat->op_container == container) {
			seat_end_mouse_operation(seat);
		}
	}
}

static void set_fullscreen_iterator(struct sway_container *con, void *data) {
	if (con->type != C_VIEW) {
		return;
	}
	if (con->sway_view->impl->set_fullscreen) {
		bool *enable = data;
		con->sway_view->impl->set_fullscreen(con->sway_view, *enable);
	}
}

void container_set_fullscreen(struct sway_container *container, bool enable) {
	if (container->is_fullscreen == enable) {
		return;
	}

	struct sway_container *workspace = container_parent(container, C_WORKSPACE);
	if (enable && workspace->sway_workspace->fullscreen) {
		container_set_fullscreen(workspace->sway_workspace->fullscreen, false);
	}

	set_fullscreen_iterator(container, &enable);
	container_for_each_child(container, set_fullscreen_iterator, &enable);

	container->is_fullscreen = enable;

	if (enable) {
		workspace->sway_workspace->fullscreen = container;
		container->saved_x = container->x;
		container->saved_y = container->y;
		container->saved_width = container->width;
		container->saved_height = container->height;

		struct sway_seat *seat;
		struct sway_container *focus, *focus_ws;
		wl_list_for_each(seat, &input_manager->seats, link) {
			focus = seat_get_focus(seat);
			if (focus) {
				focus_ws = focus;
				if (focus_ws->type != C_WORKSPACE) {
					focus_ws = container_parent(focus_ws, C_WORKSPACE);
				}
				if (focus_ws == workspace) {
					seat_set_focus(seat, container);
				}
			}
		}
	} else {
		workspace->sway_workspace->fullscreen = NULL;
		if (container_is_floating(container)) {
			container->x = container->saved_x;
			container->y = container->saved_y;
			container->width = container->saved_width;
			container->height = container->saved_height;
			struct sway_container *output =
				container_floating_find_output(container);
			if (!container_has_ancestor(container, output)) {
				container_floating_move_to_center(container);
			}
		} else {
			container->width = container->saved_width;
			container->height = container->saved_height;
		}
	}

	container_end_mouse_operation(container);

	ipc_event_window(container, "fullscreen_mode");
}

bool container_is_floating_or_child(struct sway_container *container) {
	while (container->parent && container->parent->type != C_WORKSPACE) {
		container = container->parent;
	}
	return container_is_floating(container);
}

bool container_is_fullscreen_or_child(struct sway_container *container) {
	do {
		if (container->is_fullscreen) {
			return true;
		}
		container = container->parent;
	} while (container && container->type != C_WORKSPACE);

	return false;
}

static void surface_send_enter_iterator(struct wlr_surface *surface,
		int x, int y, void *data) {
	struct wlr_output *wlr_output = data;
	wlr_surface_send_enter(surface, wlr_output);
}

static void surface_send_leave_iterator(struct wlr_surface *surface,
		int x, int y, void *data) {
	struct wlr_output *wlr_output = data;
	wlr_surface_send_leave(surface, wlr_output);
}

void container_discover_outputs(struct sway_container *con) {
	if (!sway_assert(con->type == C_CONTAINER || con->type == C_VIEW,
				"Expected a container or view")) {
		return;
	}
	struct wlr_box con_box = {
		.x = con->current.swayc_x,
		.y = con->current.swayc_y,
		.width = con->current.swayc_width,
		.height = con->current.swayc_height,
	};
	struct sway_output *old_output = container_get_effective_output(con);

	for (int i = 0; i < root_container.children->length; ++i) {
		struct sway_container *output = root_container.children->items[i];
		struct sway_output *sway_output = output->sway_output;
		struct wlr_box output_box;
		container_get_box(output, &output_box);
		struct wlr_box intersection;
		bool intersects =
			wlr_box_intersection(&con_box, &output_box, &intersection);
		int index = list_find(con->outputs, sway_output);

		if (intersects && index == -1) {
			// Send enter
			wlr_log(WLR_DEBUG, "Con %p entered output %p", con, sway_output);
			if (con->type == C_VIEW) {
				view_for_each_surface(con->sway_view,
						surface_send_enter_iterator, sway_output->wlr_output);
			}
			list_add(con->outputs, sway_output);
		} else if (!intersects && index != -1) {
			// Send leave
			wlr_log(WLR_DEBUG, "Con %p left output %p", con, sway_output);
			if (con->type == C_VIEW) {
				view_for_each_surface(con->sway_view,
					surface_send_leave_iterator, sway_output->wlr_output);
			}
			list_del(con->outputs, index);
		}
	}
	struct sway_output *new_output = container_get_effective_output(con);
	double old_scale = old_output ? old_output->wlr_output->scale : -1;
	double new_scale = new_output ? new_output->wlr_output->scale : -1;
	if (old_scale != new_scale) {
		container_update_title_textures(con);
		if (con->type == C_VIEW) {
			view_update_marks_textures(con->sway_view);
		}
	}
}

void container_remove_gaps(struct sway_container *c) {
	if (!sway_assert(c->type == C_CONTAINER || c->type == C_VIEW,
				"Expected a container or view")) {
		return;
	}
	if (c->current_gaps == 0) {
		return;
	}

	c->width += c->current_gaps * 2;
	c->height += c->current_gaps * 2;
	c->x -= c->current_gaps;
	c->y -= c->current_gaps;
	c->current_gaps = 0;
}

void container_add_gaps(struct sway_container *c) {
	if (!sway_assert(c->type == C_CONTAINER || c->type == C_VIEW,
				"Expected a container or view")) {
		return;
	}
	if (c->current_gaps > 0) {
		return;
	}
	// Linear containers don't have gaps because it'd create double gaps
	if (c->type == C_CONTAINER &&
			c->layout != L_TABBED && c->layout != L_STACKED) {
		return;
	}
	// Children of tabbed/stacked containers re-use the gaps of the container
	enum sway_container_layout layout = c->parent->layout;
	if (layout == L_TABBED || layout == L_STACKED) {
		return;
	}

	struct sway_container *ws = container_parent(c, C_WORKSPACE);

	c->current_gaps = ws->has_gaps ? ws->gaps_inner : config->gaps_inner;
	c->x += c->current_gaps;
	c->y += c->current_gaps;
	c->width -= 2 * c->current_gaps;
	c->height -= 2 * c->current_gaps;
}

int container_sibling_index(const struct sway_container *child) {
	return list_find(child->parent->children, child);
}

void container_handle_fullscreen_reparent(struct sway_container *con,
		struct sway_container *old_parent) {
	if (!con->is_fullscreen) {
		return;
	}
	struct sway_container *old_workspace = old_parent;
	if (old_workspace && old_workspace->type != C_WORKSPACE) {
		old_workspace = container_parent(old_workspace, C_WORKSPACE);
	}
	struct sway_container *new_workspace = container_parent(con, C_WORKSPACE);
	if (old_workspace == new_workspace) {
		return;
	}
	// Unmark the old workspace as fullscreen
	if (old_workspace) {
		old_workspace->sway_workspace->fullscreen = NULL;
	}

	// Mark the new workspace as fullscreen
	if (new_workspace->sway_workspace->fullscreen) {
		container_set_fullscreen(
				new_workspace->sway_workspace->fullscreen, false);
	}
	new_workspace->sway_workspace->fullscreen = con;

	// Resize container to new output dimensions
	struct sway_container *output = new_workspace->parent;
	con->x = output->x;
	con->y = output->y;
	con->width = output->width;
	con->height = output->height;

	if (con->type == C_VIEW) {
		struct sway_view *view = con->sway_view;
		view->x = output->x;
		view->y = output->y;
		view->width = output->width;
		view->height = output->height;
	} else {
		arrange_windows(new_workspace);
	}
}

void container_insert_child(struct sway_container *parent,
		struct sway_container *child, int i) {
	struct sway_container *old_parent = child->parent;
	if (old_parent) {
		container_remove_child(child);
	}
	wlr_log(WLR_DEBUG, "Inserting id:%zd at index %d", child->id, i);
	list_insert(parent->children, i, child);
	child->parent = parent;
	container_handle_fullscreen_reparent(child, old_parent);
}

struct sway_container *container_add_sibling(struct sway_container *fixed,
		struct sway_container *active) {
	// TODO handle floating
	struct sway_container *old_parent = NULL;
	if (active->parent) {
		old_parent = active->parent;
		container_remove_child(active);
	}
	struct sway_container *parent = fixed->parent;
	int i = container_sibling_index(fixed);
	list_insert(parent->children, i + 1, active);
	active->parent = parent;
	container_handle_fullscreen_reparent(active, old_parent);
	return active->parent;
}

void container_add_child(struct sway_container *parent,
		struct sway_container *child) {
	wlr_log(WLR_DEBUG, "Adding %p (%d, %fx%f) to %p (%d, %fx%f)",
			child, child->type, child->width, child->height,
			parent, parent->type, parent->width, parent->height);
	struct sway_container *old_parent = child->parent;
	list_add(parent->children, child);
	child->parent = parent;
	container_handle_fullscreen_reparent(child, old_parent);
	if (old_parent) {
		container_set_dirty(old_parent);
	}
	container_set_dirty(child);
}

struct sway_container *container_remove_child(struct sway_container *child) {
	if (child->is_fullscreen) {
		struct sway_container *workspace = container_parent(child, C_WORKSPACE);
		workspace->sway_workspace->fullscreen = NULL;
	}

	struct sway_container *parent = child->parent;
	list_t *list = container_is_floating(child) ?
		parent->sway_workspace->floating : parent->children;
	int index = list_find(list, child);
	if (index != -1) {
		list_del(list, index);
	}
	child->parent = NULL;
	container_notify_subtree_changed(parent);

	container_set_dirty(parent);
	container_set_dirty(child);

	return parent;
}

enum sway_container_layout container_get_default_layout(
		struct sway_container *con) {
	if (con->type != C_OUTPUT) {
		con = container_parent(con, C_OUTPUT);
	}

	if (!sway_assert(con != NULL,
			"container_get_default_layout must be called on an attached"
			" container below the root container")) {
		return 0;
	}

	if (config->default_layout != L_NONE) {
		return config->default_layout;
	} else if (config->default_orientation != L_NONE) {
		return config->default_orientation;
	} else if (con->width >= con->height) {
		return L_HORIZ;
	} else {
		return L_VERT;
	}
}

struct sway_container *container_replace_child(struct sway_container *child,
		struct sway_container *new_child) {
	struct sway_container *parent = child->parent;
	if (parent == NULL) {
		return NULL;
	}

	list_t *list = container_is_floating(child) ?
		parent->sway_workspace->floating : parent->children;
	int i = list_find(list, child);

	if (new_child->parent) {
		container_remove_child(new_child);
	}
	list->items[i] = new_child;
	new_child->parent = parent;
	child->parent = NULL;

	// Set geometry for new child
	new_child->x = child->x;
	new_child->y = child->y;
	new_child->width = child->width;
	new_child->height = child->height;

	// reset geometry for child
	child->width = 0;
	child->height = 0;

	return parent;
}

struct sway_container *container_split(struct sway_container *child,
		enum sway_container_layout layout) {
	// TODO floating: cannot split a floating container
	if (!sway_assert(child, "child cannot be null")) {
		return NULL;
	}
	if (child->type == C_WORKSPACE && child->children->length == 0) {
		// Special case: this just behaves like splitt
		child->prev_split_layout = child->layout;
		child->layout = layout;
		return child;
	}

	struct sway_container *cont = container_create(C_CONTAINER);

	wlr_log(WLR_DEBUG, "creating container %p around %p", cont, child);

	cont->prev_split_layout = L_NONE;
	cont->width = child->width;
	cont->height = child->height;
	cont->x = child->x;
	cont->y = child->y;
	cont->current_gaps = child->current_gaps;

	struct sway_seat *seat = input_manager_get_default_seat(input_manager);
	bool set_focus = (seat_get_focus(seat) == child);

	if (child->type == C_WORKSPACE) {
		struct sway_container *workspace = child;
		while (workspace->children->length) {
			struct sway_container *ws_child = workspace->children->items[0];
			container_remove_child(ws_child);
			container_add_child(cont, ws_child);
		}

		container_add_child(workspace, cont);
		enum sway_container_layout old_layout = workspace->layout;
		workspace->layout = layout;
		cont->layout = old_layout;
	} else {
		cont->layout = layout;
		container_replace_child(child, cont);
		container_add_child(cont, child);
	}

	if (set_focus) {
		seat_set_focus(seat, cont);
		seat_set_focus(seat, child);
	}

	container_notify_subtree_changed(cont);
	return cont;
}
