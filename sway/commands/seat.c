#include <string.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/input/input-manager.h"
#include "log.h"
#include "stringop.h"

// must be in order for the bsearch
static struct cmd_handler seat_handlers[] = {
	{ "attach", seat_cmd_attach },
	{ "cursor", seat_cmd_cursor },
	{ "fallback", seat_cmd_fallback },
	{ "hide_cursor", seat_cmd_hide_cursor },
};

struct cmd_results *cmd_seat(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "seat", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	config->handler_context.seat_config = new_seat_config(argv[0]);
	if (!config->handler_context.seat_config) {
		return cmd_results_new(CMD_FAILURE, NULL,
				"Couldn't allocate config");
	}

	struct cmd_results *res = config_subcommand(argv + 1, argc - 1,
			seat_handlers, sizeof(seat_handlers));
	if (res && res->status != CMD_SUCCESS) {
		free_seat_config(config->handler_context.seat_config);
		config->handler_context.seat_config = NULL;
		return res;
	}

	struct seat_config *sc =
		store_seat_config(config->handler_context.seat_config);
	if (!config->reading) {
		input_manager_apply_seat_config(sc);
	}

	config->handler_context.seat_config = NULL;
	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}
