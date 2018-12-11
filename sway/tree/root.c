#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output_layout.h>
#include "sway/desktop/transaction.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "list.h"
#include "log.h"
#include "util.h"

struct sway_root *root;

static void output_layout_handle_change(struct wl_listener *listener,
		void *data) {
	arrange_root();
	transaction_commit_dirty();
}

struct sway_root *root_create(void) {
	struct sway_root *root = calloc(1, sizeof(struct sway_root));
	if (!root) {
		wlr_log(WLR_ERROR, "Unable to allocate sway_root");
		return NULL;
	}
	node_init(&root->node, N_ROOT, root);
	root->output_layout = wlr_output_layout_create();
	wl_list_init(&root->all_outputs);
#if HAVE_XWAYLAND
	wl_list_init(&root->xwayland_unmanaged);
#endif
	wl_list_init(&root->drag_icons);
	wl_signal_init(&root->events.new_node);
	root->outputs = create_list();
	root->scratchpad = create_list();
	root->saved_workspaces = create_list();

	root->output_layout_change.notify = output_layout_handle_change;
	wl_signal_add(&root->output_layout->events.change,
		&root->output_layout_change);
	return root;
}

void root_destroy(struct sway_root *root) {
	wl_list_remove(&root->output_layout_change.link);
	list_free(root->scratchpad);
	list_free(root->saved_workspaces);
	list_free(root->outputs);
	wlr_output_layout_destroy(root->output_layout);
	free(root);
}

void root_scratchpad_add_container(struct sway_container *con) {
	if (!sway_assert(!con->scratchpad, "Container is already in scratchpad")) {
		return;
	}

	struct sway_container *parent = con->parent;
	struct sway_workspace *workspace = con->workspace;
	container_set_floating(con, true);
	container_detach(con);
	con->scratchpad = true;
	list_add(root->scratchpad, con);

	struct sway_seat *seat = input_manager_current_seat();
	struct sway_node *new_focus = NULL;
	if (parent) {
		arrange_container(parent);
		new_focus = seat_get_focus_inactive(seat, &parent->node);
	}
	if (!new_focus) {
		arrange_workspace(workspace);
		new_focus = seat_get_focus_inactive(seat, &workspace->node);
	}
	seat_set_focus(seat, new_focus);

	ipc_event_window(con, "move");
}

void root_scratchpad_remove_container(struct sway_container *con) {
	if (!sway_assert(con->scratchpad, "Container is not in scratchpad")) {
		return;
	}
	con->scratchpad = false;
	int index = list_find(root->scratchpad, con);
	if (index != -1) {
		list_del(root->scratchpad, index);
		ipc_event_window(con, "move");
	}
}

void root_scratchpad_show(struct sway_container *con) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *new_ws = seat_get_focused_workspace(seat);
	struct sway_workspace *old_ws = con->workspace;

	// If the current con or any of its parents are in fullscreen mode, we
	// first need to disable it before showing the scratchpad con.
	if (new_ws->fullscreen) {
		container_set_fullscreen(new_ws->fullscreen, false);
	}

	// Show the container
	if (old_ws) {
		container_detach(con);
	}
	workspace_add_floating(new_ws, con);

	// Make sure the container's center point overlaps this workspace
	double center_lx = con->x + con->width / 2;
	double center_ly = con->y + con->height / 2;

	struct wlr_box workspace_box;
	workspace_get_box(new_ws, &workspace_box);
	if (!wlr_box_contains_point(&workspace_box, center_lx, center_ly)) {
		// Maybe resize it
		if (con->width > new_ws->width || con->height > new_ws->height) {
			container_init_floating(con);
		}

		// Center it
		double new_lx = new_ws->x + (new_ws->width - con->width) / 2;
		double new_ly = new_ws->y + (new_ws->height - con->height) / 2;
		container_floating_move_to(con, new_lx, new_ly);
	}

	arrange_workspace(new_ws);
	seat_set_focus(seat, seat_get_focus_inactive(seat, &con->node));

	if (new_ws != old_ws) {
		ipc_event_window(con, "move");
	}
}

void root_scratchpad_hide(struct sway_container *con) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_node *focus = seat_get_focus(seat);
	struct sway_workspace *ws = con->workspace;

	container_detach(con);
	arrange_workspace(ws);
	if (&con->node == focus) {
		seat_set_focus(seat, seat_get_focus_inactive(seat, &ws->node));
	}
	list_move_to_end(root->scratchpad, con);

	ipc_event_window(con, "move");
}

struct pid_workspace {
	pid_t pid;
	char *workspace;
	struct timespec time_added;

	struct sway_output *output;
	struct wl_listener output_destroy;

	struct wl_list link;
};

static struct wl_list pid_workspaces;

struct sway_workspace *root_workspace_for_pid(pid_t pid) {
	if (!pid_workspaces.prev && !pid_workspaces.next) {
		wl_list_init(&pid_workspaces);
		return NULL;
	}

	struct sway_workspace *ws = NULL;
	struct pid_workspace *pw = NULL;

	wlr_log(WLR_DEBUG, "Looking up workspace for pid %d", pid);

	do {
		struct pid_workspace *_pw = NULL;
		wl_list_for_each(_pw, &pid_workspaces, link) {
			if (pid == _pw->pid) {
				pw = _pw;
				wlr_log(WLR_DEBUG,
						"found pid_workspace for pid %d, workspace %s",
						pid, pw->workspace);
				goto found;
			}
		}
		pid = get_parent_pid(pid);
	} while (pid > 1);
found:

	if (pw && pw->workspace) {
		ws = workspace_by_name(pw->workspace);

		if (!ws) {
			wlr_log(WLR_DEBUG,
					"Creating workspace %s for pid %d because it disappeared",
					pw->workspace, pid);
			ws = workspace_create(pw->output, pw->workspace);
		}

		wl_list_remove(&pw->output_destroy.link);
		wl_list_remove(&pw->link);
		free(pw->workspace);
		free(pw);
	}

	return ws;
}

static void pw_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct pid_workspace *pw = wl_container_of(listener, pw, output_destroy);
	pw->output = NULL;
	wl_list_remove(&pw->output_destroy.link);
	wl_list_init(&pw->output_destroy.link);
}

void root_record_workspace_pid(pid_t pid) {
	wlr_log(WLR_DEBUG, "Recording workspace for process %d", pid);
	if (!pid_workspaces.prev && !pid_workspaces.next) {
		wl_list_init(&pid_workspaces);
	}

	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *ws = seat_get_focused_workspace(seat);
	if (!ws) {
		wlr_log(WLR_DEBUG, "Bailing out, no workspace");
		return;
	}
	struct sway_output *output = ws->output;
	if (!output) {
		wlr_log(WLR_DEBUG, "Bailing out, no output");
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	// Remove expired entries
	static const int timeout = 60;
	struct pid_workspace *old, *_old;
	wl_list_for_each_safe(old, _old, &pid_workspaces, link) {
		if (now.tv_sec - old->time_added.tv_sec >= timeout) {
			wl_list_remove(&old->output_destroy.link);
			wl_list_remove(&old->link);
			free(old->workspace);
			free(old);
		}
	}

	struct pid_workspace *pw = calloc(1, sizeof(struct pid_workspace));
	pw->workspace = strdup(ws->name);
	pw->output = output;
	pw->pid = pid;
	memcpy(&pw->time_added, &now, sizeof(struct timespec));
	pw->output_destroy.notify = pw_handle_output_destroy;
	wl_signal_add(&output->wlr_output->events.destroy, &pw->output_destroy);
	wl_list_insert(&pid_workspaces, &pw->link);
}

void root_for_each_workspace(void (*f)(struct sway_workspace *ws, void *data),
		void *data) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_for_each_workspace(output, f, data);
	}
}

void root_for_each_container(void (*f)(struct sway_container *con, void *data),
		void *data) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_for_each_container(output, f, data);
	}

	// Scratchpad
	for (int i = 0; i < root->scratchpad->length; ++i) {
		struct sway_container *container = root->scratchpad->items[i];
		// If the container has a workspace then it's visible on a workspace
		// and will have been iterated in the previous for loop. So we only
		// iterate the hidden scratchpad containers here.
		if (!container->workspace) {
			f(container, data);
			container_for_each_child(container, f, data);
		}
	}

	// Saved workspaces
	for (int i = 0; i < root->saved_workspaces->length; ++i) {
		struct sway_workspace *ws = root->saved_workspaces->items[i];
		workspace_for_each_container(ws, f, data);
	}
}

struct sway_output *root_find_output(
		bool (*test)(struct sway_output *output, void *data), void *data) {
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		if (test(output, data)) {
			return output;
		}
	}
	return NULL;
}

struct sway_workspace *root_find_workspace(
		bool (*test)(struct sway_workspace *ws, void *data), void *data) {
	struct sway_workspace *result = NULL;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		if ((result = output_find_workspace(output, test, data))) {
			return result;
		}
	}
	return NULL;
}

struct sway_container *root_find_container(
		bool (*test)(struct sway_container *con, void *data), void *data) {
	struct sway_container *result = NULL;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		if ((result = output_find_container(output, test, data))) {
			return result;
		}
	}

	// Scratchpad
	for (int i = 0; i < root->scratchpad->length; ++i) {
		struct sway_container *container = root->scratchpad->items[i];
		if (!container->workspace) {
			if (test(container, data)) {
				return container;
			}
			if ((result = container_find_child(container, test, data))) {
				return result;
			}
		}
	}

	// Saved workspaces
	for (int i = 0; i < root->saved_workspaces->length; ++i) {
		struct sway_workspace *ws = root->saved_workspaces->items[i];
		if ((result = workspace_find_container(ws, test, data))) {
			return result;
		}
	}

	return NULL;
}

void root_get_box(struct sway_root *root, struct wlr_box *box) {
	box->x = root->x;
	box->y = root->y;
	box->width = root->width;
	box->height = root->height;
}
