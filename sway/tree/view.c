#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <strings.h>
#include <wayland-server.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include "config.h"
#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include "list.h"
#include "log.h"
#include "sway/criteria.h"
#include "sway/commands.h"
#include "sway/desktop.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/input/seat.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "sway/config.h"
#include "sway/xdg_decoration.h"
#include "pango.h"
#include "stringop.h"

void view_init(struct sway_view *view, enum sway_view_type type,
		const struct sway_view_impl *impl) {
	view->type = type;
	view->impl = impl;
	view->executed_criteria = create_list();
	view->allow_request_urgent = true;
	wl_signal_init(&view->events.unmap);
}

void view_destroy(struct sway_view *view) {
	if (!sway_assert(view->surface == NULL, "Tried to free mapped view")) {
		return;
	}
	if (!sway_assert(view->destroying,
				"Tried to free view which wasn't marked as destroying")) {
		return;
	}
	if (!sway_assert(view->container == NULL,
				"Tried to free view which still has a container "
				"(might have a pending transaction?)")) {
		return;
	}
	list_free(view->executed_criteria);

	free(view->title_format);

	if (view->impl->destroy) {
		view->impl->destroy(view);
	} else {
		free(view);
	}
}

void view_begin_destroy(struct sway_view *view) {
	if (!sway_assert(view->surface == NULL, "Tried to destroy a mapped view")) {
		return;
	}
	view->destroying = true;

	if (!view->container) {
		view_destroy(view);
	}
}

const char *view_get_title(struct sway_view *view) {
	if (view->impl->get_string_prop) {
		return view->impl->get_string_prop(view, VIEW_PROP_TITLE);
	}
	return NULL;
}

const char *view_get_app_id(struct sway_view *view) {
	if (view->impl->get_string_prop) {
		return view->impl->get_string_prop(view, VIEW_PROP_APP_ID);
	}
	return NULL;
}

const char *view_get_class(struct sway_view *view) {
	if (view->impl->get_string_prop) {
		return view->impl->get_string_prop(view, VIEW_PROP_CLASS);
	}
	return NULL;
}

const char *view_get_instance(struct sway_view *view) {
	if (view->impl->get_string_prop) {
		return view->impl->get_string_prop(view, VIEW_PROP_INSTANCE);
	}
	return NULL;
}
#if HAVE_XWAYLAND
uint32_t view_get_x11_window_id(struct sway_view *view) {
	if (view->impl->get_int_prop) {
		return view->impl->get_int_prop(view, VIEW_PROP_X11_WINDOW_ID);
	}
	return 0;
}

uint32_t view_get_x11_parent_id(struct sway_view *view) {
	if (view->impl->get_int_prop) {
		return view->impl->get_int_prop(view, VIEW_PROP_X11_PARENT_ID);
	}
	return 0;
}
#endif
const char *view_get_window_role(struct sway_view *view) {
	if (view->impl->get_string_prop) {
		return view->impl->get_string_prop(view, VIEW_PROP_WINDOW_ROLE);
	}
	return NULL;
}

uint32_t view_get_window_type(struct sway_view *view) {
	if (view->impl->get_int_prop) {
		return view->impl->get_int_prop(view, VIEW_PROP_WINDOW_TYPE);
	}
	return 0;
}

const char *view_get_shell(struct sway_view *view) {
	switch(view->type) {
	case SWAY_VIEW_XDG_SHELL_V6:
		return "xdg_shell_v6";
	case SWAY_VIEW_XDG_SHELL:
		return "xdg_shell";
#if HAVE_XWAYLAND
	case SWAY_VIEW_XWAYLAND:
		return "xwayland";
#endif
	}
	return "unknown";
}

void view_get_constraints(struct sway_view *view, double *min_width,
		double *max_width, double *min_height, double *max_height) {
	if (view->impl->get_constraints) {
		view->impl->get_constraints(view,
				min_width, max_width, min_height, max_height);
	} else {
		*min_width = DBL_MIN;
		*max_width = DBL_MAX;
		*min_height = DBL_MIN;
		*max_height = DBL_MAX;
	}
}

uint32_t view_configure(struct sway_view *view, double lx, double ly, int width,
		int height) {
	if (view->impl->configure) {
		return view->impl->configure(view, lx, ly, width, height);
	}
	return 0;
}

bool view_is_only_visible(struct sway_view *view) {
	bool only_view = true;
	struct sway_container *con = view->container;
	while (con) {
		enum sway_container_layout layout = container_parent_layout(con);
		if (layout != L_TABBED && layout != L_STACKED) {
			list_t *siblings = container_get_siblings(con);
			if (siblings && siblings->length > 1) {
				only_view = false;
				break;
			}
		}
		con = con->parent;
	}
	return only_view;
}

static bool gaps_to_edge(struct sway_view *view) {
	struct sway_container *con = view->container;
	while (con) {
		if (con->current_gaps.top > 0 || con->current_gaps.right > 0 ||
				con->current_gaps.bottom > 0 || con->current_gaps.left > 0) {
			return true;
		}
		con = con->parent;
	}
	struct side_gaps gaps = view->container->workspace->current_gaps;
	return gaps.top > 0 || gaps.right > 0 || gaps.bottom > 0 || gaps.left > 0;
}

void view_autoconfigure(struct sway_view *view) {
	struct sway_container *con = view->container;
	if (!con->workspace) {
		// Hidden in the scratchpad
		return;
	}
	struct sway_output *output = con->workspace->output;

	if (con->is_fullscreen) {
		con->content_x = output->lx;
		con->content_y = output->ly;
		con->content_width = output->width;
		con->content_height = output->height;
		return;
	}

	struct sway_workspace *ws = view->container->workspace;

	bool smart = config->hide_edge_borders == E_SMART ||
		config->hide_edge_borders == E_SMART_NO_GAPS;
	bool other_views = smart && !view_is_only_visible(view);
	bool no_gaps = config->hide_edge_borders != E_SMART_NO_GAPS
		|| !gaps_to_edge(view);

	con->border_top = con->border_bottom = true;
	con->border_left = con->border_right = true;
	if (config->hide_edge_borders == E_BOTH
			|| config->hide_edge_borders == E_VERTICAL
			|| (smart && !other_views && no_gaps)) {
		con->border_left = con->x - con->current_gaps.left != ws->x;
		int right_x = con->x + con->width + con->current_gaps.right;
		con->border_right = right_x != ws->x + ws->width;
	}
	if (config->hide_edge_borders == E_BOTH
			|| config->hide_edge_borders == E_HORIZONTAL
			|| (smart && !other_views && no_gaps)) {
		con->border_top = con->y - con->current_gaps.top != ws->y;
		int bottom_y = con->y + con->height + con->current_gaps.bottom;
		con->border_bottom = bottom_y != ws->y + ws->height;
	}

	double x, y, width, height;
	x = y = width = height = 0;
	double y_offset = 0;

	// In a tabbed or stacked container, the container's y is the top of the
	// title area. We have to offset the surface y by the height of the title,
	// bar, and disable any top border because we'll always have the title bar.
	enum sway_container_layout layout = container_parent_layout(con);
	if (layout == L_TABBED && !container_is_floating(con)) {
		y_offset = container_titlebar_height();
		con->border_top = false;
	} else if (layout == L_STACKED && !container_is_floating(con)) {
		list_t *siblings = container_get_siblings(con);
		y_offset = container_titlebar_height() * siblings->length;
		con->border_top = false;
	}

	switch (con->border) {
	case B_CSD:
	case B_NONE:
		x = con->x;
		y = con->y + y_offset;
		width = con->width;
		height = con->height - y_offset;
		break;
	case B_PIXEL:
		x = con->x + con->border_thickness * con->border_left;
		y = con->y + con->border_thickness * con->border_top + y_offset;
		width = con->width
			- con->border_thickness * con->border_left
			- con->border_thickness * con->border_right;
		height = con->height - y_offset
			- con->border_thickness * con->border_top
			- con->border_thickness * con->border_bottom;
		break;
	case B_NORMAL:
		// Height is: 1px border + 3px pad + title height + 3px pad + 1px border
		x = con->x + con->border_thickness * con->border_left;
		width = con->width
			- con->border_thickness * con->border_left
			- con->border_thickness * con->border_right;
		if (y_offset) {
			y = con->y + y_offset;
			height = con->height - y_offset
				- con->border_thickness * con->border_bottom;
		} else {
			y = con->y + container_titlebar_height();
			height = con->height - container_titlebar_height()
				- con->border_thickness * con->border_bottom;
		}
		break;
	}

	con->content_x = x;
	con->content_y = y;
	con->content_width = width;
	con->content_height = height;
}

void view_set_activated(struct sway_view *view, bool activated) {
	if (view->impl->set_activated) {
		view->impl->set_activated(view, activated);
	}
}

void view_request_activate(struct sway_view *view) {
	struct sway_workspace *ws = view->container->workspace;
	if (!ws) { // hidden scratchpad container
		return;
	}
	struct sway_seat *seat = input_manager_current_seat();

	switch (config->focus_on_window_activation) {
	case FOWA_SMART:
		if (workspace_is_visible(ws)) {
			seat_set_focus_container(seat, view->container);
		} else {
			view_set_urgent(view, true);
		}
		break;
	case FOWA_URGENT:
		view_set_urgent(view, true);
		break;
	case FOWA_FOCUS:
		seat_set_focus_container(seat, view->container);
		break;
	case FOWA_NONE:
		break;
	}
}

void view_set_csd_from_server(struct sway_view *view, bool enabled) {
	wlr_log(WLR_DEBUG, "Telling view %p to set CSD to %i", view, enabled);
	if (view->xdg_decoration) {
		uint32_t mode = enabled ?
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE :
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
		wlr_xdg_toplevel_decoration_v1_set_mode(
				view->xdg_decoration->wlr_xdg_decoration, mode);
	}
	view->using_csd = enabled;
}

void view_update_csd_from_client(struct sway_view *view, bool enabled) {
	wlr_log(WLR_DEBUG, "View %p updated CSD to %i", view, enabled);
	struct sway_container *con = view->container;
	if (enabled && con && con->border != B_CSD) {
		con->saved_border = con->border;
		if (container_is_floating(con)) {
			con->border = B_CSD;
		}
	} else if (!enabled && con && con->border == B_CSD) {
		con->border = con->saved_border;
	}
	view->using_csd = enabled;
}

void view_set_tiled(struct sway_view *view, bool tiled) {
	if (view->impl->set_tiled) {
		view->impl->set_tiled(view, tiled);
	}
}

void view_close(struct sway_view *view) {
	if (view->impl->close) {
		view->impl->close(view);
	}
}

void view_close_popups(struct sway_view *view) {
	if (view->impl->close_popups) {
		view->impl->close_popups(view);
	}
}

void view_damage_from(struct sway_view *view) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_damage_from_view(output, view);
	}
}

void view_for_each_surface(struct sway_view *view,
		wlr_surface_iterator_func_t iterator, void *user_data) {
	if (!view->surface) {
		return;
	}
	if (view->impl->for_each_surface) {
		view->impl->for_each_surface(view, iterator, user_data);
	} else {
		wlr_surface_for_each_surface(view->surface, iterator, user_data);
	}
}

void view_for_each_popup(struct sway_view *view,
		wlr_surface_iterator_func_t iterator, void *user_data) {
	if (!view->surface) {
		return;
	}
	if (view->impl->for_each_popup) {
		view->impl->for_each_popup(view, iterator, user_data);
	}
}

static void view_subsurface_create(struct sway_view *view,
	struct wlr_subsurface *subsurface);

static void view_init_subsurfaces(struct sway_view *view,
	struct wlr_surface *surface);

static void view_handle_surface_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct sway_view *view =
		wl_container_of(listener, view, surface_new_subsurface);
	struct wlr_subsurface *subsurface = data;
	view_subsurface_create(view, subsurface);
}

static bool view_has_executed_criteria(struct sway_view *view,
		struct criteria *criteria) {
	for (int i = 0; i < view->executed_criteria->length; ++i) {
		struct criteria *item = view->executed_criteria->items[i];
		if (item == criteria) {
			return true;
		}
	}
	return false;
}

void view_execute_criteria(struct sway_view *view) {
	list_t *criterias = criteria_for_view(view, CT_COMMAND);
	for (int i = 0; i < criterias->length; i++) {
		struct criteria *criteria = criterias->items[i];
		wlr_log(WLR_DEBUG, "Checking criteria %s", criteria->raw);
		if (view_has_executed_criteria(view, criteria)) {
			wlr_log(WLR_DEBUG, "Criteria already executed");
			continue;
		}
		wlr_log(WLR_DEBUG, "for_window '%s' matches view %p, cmd: '%s'",
				criteria->raw, view, criteria->cmdlist);
		list_add(view->executed_criteria, criteria);
		list_t *res_list = execute_command(
				criteria->cmdlist, NULL, view->container);
		while (res_list->length) {
			struct cmd_results *res = res_list->items[0];
			free_cmd_results(res);
			list_del(res_list, 0);
		}
		list_free(res_list);
	}
	list_free(criterias);
}

static struct sway_workspace *select_workspace(struct sway_view *view) {
	struct sway_seat *seat = input_manager_current_seat();

	// Check if there's any `assign` criteria for the view
	list_t *criterias = criteria_for_view(view,
			CT_ASSIGN_WORKSPACE | CT_ASSIGN_WORKSPACE_NUMBER | CT_ASSIGN_OUTPUT);
	struct sway_workspace *ws = NULL;
	for (int i = 0; i < criterias->length; ++i) {
		struct criteria *criteria = criterias->items[i];
		if (criteria->type == CT_ASSIGN_OUTPUT) {
			struct sway_output *output = output_by_name_or_id(criteria->target);
			if (output) {
				ws = output_get_active_workspace(output);
				break;
			}
		} else {
			// CT_ASSIGN_WORKSPACE(_NUMBER)
			ws = criteria->type == CT_ASSIGN_WORKSPACE_NUMBER ?
				workspace_by_number(criteria->target) :
				workspace_by_name(criteria->target);

			if (!ws) {
				if (strcasecmp(criteria->target, "back_and_forth") == 0) {
					if (seat->prev_workspace_name) {
						ws = workspace_create(NULL, seat->prev_workspace_name);
					}
				} else {
					ws = workspace_create(NULL, criteria->target);
				}
			}
			break;
		}
	}
	list_free(criterias);
	if (ws) {
		return ws;
	}

	// Check if there's a PID mapping
	pid_t pid;
#if HAVE_XWAYLAND
	if (view->type == SWAY_VIEW_XWAYLAND) {
		struct wlr_xwayland_surface *surf =
			wlr_xwayland_surface_from_wlr_surface(view->surface);
		pid = surf->pid;
	} else {
		struct wl_client *client =
			wl_resource_get_client(view->surface->resource);
		wl_client_get_credentials(client, &pid, NULL, NULL);
	}
#else
	struct wl_client *client =
		wl_resource_get_client(view->surface->resource);
	wl_client_get_credentials(client, &pid, NULL, NULL);
#endif
	view->pid = pid;
	ws = root_workspace_for_pid(pid);
	if (ws) {
		return ws;
	}

	// Use the focused workspace
	struct sway_node *node = seat_get_focus_inactive(seat, &root->node);
	if (node && node->type == N_WORKSPACE) {
		return node->sway_workspace;
	} else if (node && node->type == N_CONTAINER) {
		return node->sway_container->workspace;
	}

	// If there's no focus_inactive workspace then we must be running without
	// any outputs connected
	return root->saved_workspaces->items[0];
}

static bool should_focus(struct sway_view *view) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_container *prev_con = seat_get_focused_container(seat);
	struct sway_workspace *prev_ws = seat_get_focused_workspace(seat);
	struct sway_workspace *map_ws = view->container->workspace;

	// Views can only take focus if they are mapped into the active workspace
	if (prev_ws != map_ws) {
		return false;
	}

	// If the view is the only one in the focused workspace, it'll get focus
	// regardless of any no_focus criteria.
	if (!view->container->parent && !prev_con) {
		size_t num_children = view->container->workspace->tiling->length +
			view->container->workspace->floating->length;
		if (num_children == 1) {
			return true;
		}
	}

	// Check no_focus criteria
	list_t *criterias = criteria_for_view(view, CT_NO_FOCUS);
	size_t len = criterias->length;
	list_free(criterias);
	return len == 0;
}

void view_map(struct sway_view *view, struct wlr_surface *wlr_surface,
			  bool fullscreen, bool decoration) {
	if (!sway_assert(view->surface == NULL, "cannot map mapped view")) {
		return;
	}
	view->surface = wlr_surface;

	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *ws = select_workspace(view);
	struct sway_node *node = seat_get_focus_inactive(seat, &ws->node);
	struct sway_container *target_sibling = node->type == N_CONTAINER ?
		node->sway_container : NULL;

	// If we're about to launch the view into the floating container, then
	// launch it as a tiled view in the root of the workspace instead.
	if (target_sibling && container_is_floating(target_sibling)) {
		target_sibling = NULL;
	}

	view->container = container_create(view);
	if (target_sibling) {
		container_add_sibling(target_sibling, view->container, 1);
	} else {
		workspace_add_tiling(ws, view->container);
	}
	ipc_event_window(view->container, "new");

	view_init_subsurfaces(view, wlr_surface);
	wl_signal_add(&wlr_surface->events.new_subsurface,
		&view->surface_new_subsurface);
	view->surface_new_subsurface.notify = view_handle_surface_new_subsurface;

	if (view->impl->wants_floating && view->impl->wants_floating(view)) {
		view->container->border = config->floating_border;
		view->container->border_thickness = config->floating_border_thickness;
		container_set_floating(view->container, true);
	} else {
		view->container->border = config->border;
		view->container->border_thickness = config->border_thickness;
		view_set_tiled(view, true);
	}

	if (config->popup_during_fullscreen == POPUP_LEAVE &&
			view->container->workspace &&
			view->container->workspace->fullscreen &&
			view->container->workspace->fullscreen->view) {
		struct sway_container *fs = view->container->workspace->fullscreen;
		if (view_is_transient_for(view, fs->view)) {
			container_set_fullscreen(fs, false);
		}
	}

	view_update_title(view, false);
	container_update_representation(view->container);

	if (decoration) {
		view_update_csd_from_client(view, decoration);
	}

	if (fullscreen) {
		container_set_fullscreen(view->container, true);
		arrange_workspace(view->container->workspace);
	} else {
		if (view->container->parent) {
			arrange_container(view->container->parent);
		} else if (view->container->workspace) {
			arrange_workspace(view->container->workspace);
		}
	}

	view_execute_criteria(view);

	if (should_focus(view)) {
		input_manager_set_focus(&view->container->node);
	}
}

void view_unmap(struct sway_view *view) {
	wl_signal_emit(&view->events.unmap, view);

	wl_list_remove(&view->surface_new_subsurface.link);

	if (view->urgent_timer) {
		wl_event_source_remove(view->urgent_timer);
		view->urgent_timer = NULL;
	}

	struct sway_container *parent = view->container->parent;
	struct sway_workspace *ws = view->container->workspace;
	container_begin_destroy(view->container);
	if (parent) {
		container_reap_empty(parent);
	} else if (ws) {
		workspace_consider_destroy(ws);
	}

	if (ws && !ws->node.destroying) {
		arrange_workspace(ws);
		workspace_detect_urgent(ws);
	}

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat->cursor->image_surface = NULL;
		seat_consider_warp_to_focus(seat);
	}

	transaction_commit_dirty();
	view->surface = NULL;
}

void view_update_size(struct sway_view *view, int width, int height) {
	if (!sway_assert(container_is_floating(view->container),
				"Expected a floating container")) {
		return;
	}
	view->container->content_width = width;
	view->container->content_height = height;
	view->container->current.content_width = width;
	view->container->current.content_height = height;
	container_set_geometry_from_content(view->container);
}

static const struct sway_view_child_impl subsurface_impl;

static void subsurface_get_root_coords(struct sway_view_child *child,
		int *root_sx, int *root_sy) {
	struct wlr_surface *surface = child->surface;
	*root_sx = -child->view->geometry.x;
	*root_sy = -child->view->geometry.y;

	while (surface && wlr_surface_is_subsurface(surface)) {
		struct wlr_subsurface *subsurface =
			wlr_subsurface_from_wlr_surface(surface);
		*root_sx += subsurface->current.x;
		*root_sy += subsurface->current.y;
		surface = subsurface->parent;
	}
}

static void subsurface_destroy(struct sway_view_child *child) {
	if (!sway_assert(child->impl == &subsurface_impl,
			"Expected a subsurface")) {
		return;
	}
	struct sway_subsurface *subsurface = (struct sway_subsurface *)child;
	wl_list_remove(&subsurface->destroy.link);
	free(subsurface);
}

static const struct sway_view_child_impl subsurface_impl = {
	.get_root_coords = subsurface_get_root_coords,
	.destroy = subsurface_destroy,
};

static void subsurface_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct sway_subsurface *subsurface =
		wl_container_of(listener, subsurface, destroy);
	struct sway_view_child *child = &subsurface->child;
	view_child_destroy(child);
}

static void view_child_damage(struct sway_view_child *child, bool whole);

static void view_subsurface_create(struct sway_view *view,
		struct wlr_subsurface *wlr_subsurface) {
	struct sway_subsurface *subsurface =
		calloc(1, sizeof(struct sway_subsurface));
	if (subsurface == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return;
	}
	view_child_init(&subsurface->child, &subsurface_impl, view,
		wlr_subsurface->surface);

	wl_signal_add(&wlr_subsurface->events.destroy, &subsurface->destroy);
	subsurface->destroy.notify = subsurface_handle_destroy;

	subsurface->child.mapped = true;
	view_child_damage(&subsurface->child, true);
}

static void view_child_damage(struct sway_view_child *child, bool whole) {
	int sx, sy;
	child->impl->get_root_coords(child, &sx, &sy);
	desktop_damage_surface(child->surface,
			child->view->container->content_x + sx,
			child->view->container->content_y + sy, whole);
}

static void view_child_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	struct sway_view_child *child =
		wl_container_of(listener, child, surface_commit);
	view_child_damage(child, false);
}

static void view_child_handle_surface_new_subsurface(
		struct wl_listener *listener, void *data) {
	struct sway_view_child *child =
		wl_container_of(listener, child, surface_new_subsurface);
	struct wlr_subsurface *subsurface = data;
	view_subsurface_create(child->view, subsurface);
}

static void view_child_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct sway_view_child *child =
		wl_container_of(listener, child, surface_destroy);
	view_child_destroy(child);
}

static void view_init_subsurfaces(struct sway_view *view,
		struct wlr_surface *surface) {
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->subsurfaces, parent_link) {
		view_subsurface_create(view, subsurface);
	}
}

static void view_child_handle_surface_map(struct wl_listener *listener,
		void *data) {
	struct sway_view_child *child =
		wl_container_of(listener, child, surface_map);
	child->mapped = true;
	view_child_damage(child, true);
}

static void view_child_handle_surface_unmap(struct wl_listener *listener,
		void *data) {
	struct sway_view_child *child =
		wl_container_of(listener, child, surface_unmap);
	view_child_damage(child, true);
	child->mapped = false;
}

void view_child_init(struct sway_view_child *child,
		const struct sway_view_child_impl *impl, struct sway_view *view,
		struct wlr_surface *surface) {
	child->impl = impl;
	child->view = view;
	child->surface = surface;

	wl_signal_add(&surface->events.commit, &child->surface_commit);
	child->surface_commit.notify = view_child_handle_surface_commit;
	wl_signal_add(&surface->events.new_subsurface,
		&child->surface_new_subsurface);
	child->surface_new_subsurface.notify =
		view_child_handle_surface_new_subsurface;
	wl_signal_add(&surface->events.destroy, &child->surface_destroy);
	child->surface_destroy.notify = view_child_handle_surface_destroy;

	// Not all child views have a map/unmap event
	child->surface_map.notify = view_child_handle_surface_map;
	child->surface_unmap.notify = view_child_handle_surface_unmap;

	struct sway_output *output = child->view->container->workspace->output;
	wlr_surface_send_enter(child->surface, output->wlr_output);

	view_init_subsurfaces(child->view, surface);
}

void view_child_destroy(struct sway_view_child *child) {
	if (child->mapped && child->view->container != NULL) {
		view_child_damage(child, true);
	}

	wl_list_remove(&child->surface_commit.link);
	wl_list_remove(&child->surface_destroy.link);

	if (child->impl && child->impl->destroy) {
		child->impl->destroy(child);
	} else {
		free(child);
	}
}

struct sway_view *view_from_wlr_surface(struct wlr_surface *wlr_surface) {
	if (wlr_surface_is_xdg_surface(wlr_surface)) {
		struct wlr_xdg_surface *xdg_surface =
			wlr_xdg_surface_from_wlr_surface(wlr_surface);
		return view_from_wlr_xdg_surface(xdg_surface);
	}
	if (wlr_surface_is_xdg_surface_v6(wlr_surface)) {
		struct wlr_xdg_surface_v6 *xdg_surface_v6 =
			wlr_xdg_surface_v6_from_wlr_surface(wlr_surface);
		return view_from_wlr_xdg_surface_v6(xdg_surface_v6);
	}
#if HAVE_XWAYLAND
	if (wlr_surface_is_xwayland_surface(wlr_surface)) {
		struct wlr_xwayland_surface *xsurface =
			wlr_xwayland_surface_from_wlr_surface(wlr_surface);
		return view_from_wlr_xwayland_surface(xsurface);
	}
#endif
	if (wlr_surface_is_subsurface(wlr_surface)) {
		struct wlr_subsurface *subsurface =
			wlr_subsurface_from_wlr_surface(wlr_surface);
		return view_from_wlr_surface(subsurface->parent);
	}
	if (wlr_surface_is_layer_surface(wlr_surface)) {
		return NULL;
	}

	const char *role = wlr_surface->role ? wlr_surface->role->name : NULL;
	wlr_log(WLR_DEBUG, "Surface of unknown type (role %s): %p",
		role, wlr_surface);
	return NULL;
}

static char *escape_pango_markup(const char *buffer) {
	size_t length = escape_markup_text(buffer, NULL);
	char *escaped_title = calloc(length + 1, sizeof(char));
	escape_markup_text(buffer, escaped_title);
	return escaped_title;
}

static size_t append_prop(char *buffer, const char *value) {
	if (!value) {
		return 0;
	}
	// If using pango_markup in font, we need to escape all markup chars
	// from values to make sure tags are not inserted by clients
	if (config->pango_markup) {
		char *escaped_value = escape_pango_markup(value);
		lenient_strcat(buffer, escaped_value);
		size_t len = strlen(escaped_value);
		free(escaped_value);
		return len;
	} else {
		lenient_strcat(buffer, value);
		return strlen(value);
	}
}

/**
 * Calculate and return the length of the formatted title.
 * If buffer is not NULL, also populate the buffer with the formatted title.
 */
static size_t parse_title_format(struct sway_view *view, char *buffer) {
	if (!view->title_format || strcmp(view->title_format, "%title") == 0) {
		return append_prop(buffer, view_get_title(view));
	}

	size_t len = 0;
	char *format = view->title_format;
	char *next = strchr(format, '%');
	while (next) {
		// Copy everything up to the %
		lenient_strncat(buffer, format, next - format);
		len += next - format;
		format = next;

		if (strncmp(next, "%title", 6) == 0) {
			len += append_prop(buffer, view_get_title(view));
			format += 6;
		} else if (strncmp(next, "%app_id", 7) == 0) {
			len += append_prop(buffer, view_get_app_id(view));
			format += 7;
		} else if (strncmp(next, "%class", 6) == 0) {
			len += append_prop(buffer, view_get_class(view));
			format += 6;
		} else if (strncmp(next, "%instance", 9) == 0) {
			len += append_prop(buffer, view_get_instance(view));
			format += 9;
		} else if (strncmp(next, "%shell", 6) == 0) {
			len += append_prop(buffer, view_get_shell(view));
			format += 6;
		} else {
			lenient_strcat(buffer, "%");
			++format;
			++len;
		}
		next = strchr(format, '%');
	}
	lenient_strcat(buffer, format);
	len += strlen(format);

	return len;
}

void view_update_title(struct sway_view *view, bool force) {
	const char *title = view_get_title(view);

	if (!force) {
		if (title && view->container->title &&
				strcmp(title, view->container->title) == 0) {
			return;
		}
		if (!title && !view->container->title) {
			return;
		}
	}

	free(view->container->title);
	free(view->container->formatted_title);
	if (title) {
		size_t len = parse_title_format(view, NULL);
		char *buffer = calloc(len + 1, sizeof(char));
		if (!sway_assert(buffer, "Unable to allocate title string")) {
			return;
		}
		parse_title_format(view, buffer);

		view->container->title = strdup(title);
		view->container->formatted_title = buffer;
	} else {
		view->container->title = NULL;
		view->container->formatted_title = NULL;
	}
	container_calculate_title_height(view->container);
	config_update_font_height(false);

	// Update title after the global font height is updated
	container_update_title_textures(view->container);

	ipc_event_window(view->container, "title");
}

bool view_is_visible(struct sway_view *view) {
	if (view->container->node.destroying) {
		return false;
	}
	struct sway_workspace *workspace = view->container->workspace;
	if (!workspace) {
		return false;
	}
	// Determine if view is nested inside a floating container which is sticky
	struct sway_container *floater = view->container;
	while (floater->parent) {
		floater = floater->parent;
	}
	bool is_sticky = container_is_floating(floater) && floater->is_sticky;
	if (!is_sticky && !workspace_is_visible(workspace)) {
		return false;
	}
	// Check view isn't in a tabbed or stacked container on an inactive tab
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_container *con = view->container;
	while (con) {
		enum sway_container_layout layout = container_parent_layout(con);
		if ((layout == L_TABBED || layout == L_STACKED)
				&& !container_is_floating(con)) {
			struct sway_node *parent = con->parent ?
				&con->parent->node : &con->workspace->node;
			if (seat_get_active_tiling_child(seat, parent) != &con->node) {
				return false;
			}
		}
		con = con->parent;
	}
	// Check view isn't hidden by another fullscreen view
	if (workspace->fullscreen &&
			!container_is_fullscreen_or_child(view->container)) {
		// However, if we're transient for the fullscreen view and we allow
		// "popups" during fullscreen then it might be visible
		if (!container_is_transient_for(view->container,
					workspace->fullscreen)) {
			return false;
		}
	}
	return true;
}

void view_set_urgent(struct sway_view *view, bool enable) {
	if (view_is_urgent(view) == enable) {
		return;
	}
	if (enable) {
		struct sway_seat *seat = input_manager_current_seat();
		if (seat_get_focused_container(seat) == view->container) {
			return;
		}
		clock_gettime(CLOCK_MONOTONIC, &view->urgent);
	} else {
		view->urgent = (struct timespec){ 0 };
		if (view->urgent_timer) {
			wl_event_source_remove(view->urgent_timer);
			view->urgent_timer = NULL;
		}
	}
	container_damage_whole(view->container);

	ipc_event_window(view->container, "urgent");

	if (view->container->workspace) {
		workspace_detect_urgent(view->container->workspace);
	}
}

bool view_is_urgent(struct sway_view *view) {
	return view->urgent.tv_sec || view->urgent.tv_nsec;
}

void view_remove_saved_buffer(struct sway_view *view) {
	if (!sway_assert(view->saved_buffer, "Expected a saved buffer")) {
		return;
	}
	wlr_buffer_unref(view->saved_buffer);
	view->saved_buffer = NULL;
}

void view_save_buffer(struct sway_view *view) {
	if (!sway_assert(!view->saved_buffer, "Didn't expect saved buffer")) {
		view_remove_saved_buffer(view);
	}
	if (view->surface && wlr_surface_has_buffer(view->surface)) {
		view->saved_buffer = wlr_buffer_ref(view->surface->buffer);
		view->saved_buffer_width = view->surface->current.width;
		view->saved_buffer_height = view->surface->current.height;
	}
}

bool view_is_transient_for(struct sway_view *child,
		struct sway_view *ancestor) {
	return child->impl->is_transient_for &&
		child->impl->is_transient_for(child, ancestor);
}
