#define _XOPEN_SOURCE 500
#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/ipc-server.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "list.h"
#include "log.h"

static void do_reload(void *data) {
	// store bar ids to check against new bars for barconfig_update events
	list_t *bar_ids = create_list();
	for (int i = 0; i < config->bars->length; ++i) {
		struct bar_config *bar = config->bars->items[i];
		list_add(bar_ids, strdup(bar->id));
	}

	if (!load_main_config(config->current_config_path, true, false)) {
		wlr_log(WLR_ERROR, "Error(s) reloading config");
		list_foreach(bar_ids, free);
		list_free(bar_ids);
		return;
	}

	ipc_event_workspace(NULL, NULL, "reload");

	load_swaybars();

	for (int i = 0; i < config->bars->length; ++i) {
		struct bar_config *bar = config->bars->items[i];
		for (int j = 0; j < bar_ids->length; ++j) {
			if (strcmp(bar->id, bar_ids->items[j]) == 0) {
				ipc_event_barconfig_update(bar);
				break;
			}
		}
	}

	list_foreach(bar_ids, free);
	list_free(bar_ids);

	arrange_root();
}

struct cmd_results *cmd_reload(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "reload", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}

	if (!load_main_config(config->current_config_path, true, true)) {
		return cmd_results_new(CMD_FAILURE, "reload",
				"Error(s) reloading config.");
	}

	// The reload command frees a lot of stuff, so to avoid use-after-frees
	// we schedule the reload to happen using an idle event.
	wl_event_loop_add_idle(server.wl_event_loop, do_reload, NULL);

	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}
