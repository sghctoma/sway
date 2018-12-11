#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <pango/pangocairo.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/debug.h"
#include "sway/desktop/transaction.h"
#include "sway/server.h"
#include "sway/swaynag.h"
#include "sway/tree/root.h"
#include "sway/ipc-server.h"
#include "ipc-client.h"
#include "log.h"
#include "readline.h"
#include "stringop.h"
#include "util.h"

static bool terminate_request = false;
static int exit_value = 0;
struct sway_server server = {0};

void sway_terminate(int exit_code) {
	terminate_request = true;
	exit_value = exit_code;
	ipc_event_shutdown("exit");
	wl_display_terminate(server.wl_display);
}

void sig_handler(int signal) {
	sway_terminate(EXIT_SUCCESS);
}

void detect_raspi(void) {
	bool raspi = false;
	FILE *f = fopen("/sys/firmware/devicetree/base/model", "r");
	if (!f) {
		return;
	}
	char *line;
	while(!feof(f)) {
		if (!(line = read_line(f))) {
			break;
		}
		if (strstr(line, "Raspberry Pi")) {
			raspi = true;
		}
		free(line);
	}
	fclose(f);
	FILE *g = fopen("/proc/modules", "r");
	if (!g) {
		return;
	}
	bool vc4 = false;
	while (!feof(g)) {
		if (!(line = read_line(g))) {
			break;
		}
		if (strstr(line, "vc4")) {
			vc4 = true;
		}
		free(line);
	}
	fclose(g);
	if (!vc4 && raspi) {
		fprintf(stderr, "\x1B[1;31mWarning: You have a "
				"Raspberry Pi, but the vc4 Module is "
				"not loaded! Set 'dtoverlay=vc4-kms-v3d'"
				"in /boot/config.txt and reboot.\x1B[0m\n");
	}
}

void detect_proprietary(int allow_unsupported_gpu) {
	FILE *f = fopen("/proc/modules", "r");
	if (!f) {
		return;
	}
	while (!feof(f)) {
		char *line;
		if (!(line = read_line(f))) {
			break;
		}
		if (strstr(line, "nvidia")) {
			free(line);
			if (allow_unsupported_gpu) {
				wlr_log(WLR_ERROR,
						"!!! Proprietary Nvidia drivers are in use !!!");
			} else {
				wlr_log(WLR_ERROR,
					"Proprietary Nvidia drivers are NOT supported. "
					"Use Nouveau. To launch sway anyway, launch with "
					"--my-next-gpu-wont-be-nvidia and DO NOT report issues.");
				exit(EXIT_FAILURE);
			}
			break;
		}
		if (strstr(line, "fglrx")) {
			free(line);
			if (allow_unsupported_gpu) {
				wlr_log(WLR_ERROR,
						"!!! Proprietary AMD drivers are in use !!!");
			} else {
				wlr_log(WLR_ERROR, "Proprietary AMD drivers do NOT support "
					"Wayland. Use radeon. To try anyway, launch sway with "
					"--unsupported-gpu and DO NOT report issues.");
				exit(EXIT_FAILURE);
			}
			break;
		}
		free(line);
	}
	fclose(f);
}

void run_as_ipc_client(char *command, char *socket_path) {
	int socketfd = ipc_open_socket(socket_path);
	uint32_t len = strlen(command);
	char *resp = ipc_single_command(socketfd, IPC_COMMAND, command, &len);
	printf("%s\n", resp);
	close(socketfd);
}

static void log_env(void) {
	const char *log_vars[] = {
		"PATH",
		"LD_LIBRARY_PATH",
		"LD_PRELOAD_PATH",
		"LD_LIBRARY_PATH",
		"SWAY_CURSOR_THEME",
		"SWAY_CURSOR_SIZE",
		"SWAYSOCK"
	};
	for (size_t i = 0; i < sizeof(log_vars) / sizeof(char *); ++i) {
		wlr_log(WLR_INFO, "%s=%s", log_vars[i], getenv(log_vars[i]));
	}
}

static void log_distro(void) {
	const char *paths[] = {
		"/etc/lsb-release",
		"/etc/os-release",
		"/etc/debian_version",
		"/etc/redhat-release",
		"/etc/gentoo-release",
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(char *); ++i) {
		FILE *f = fopen(paths[i], "r");
		if (f) {
			wlr_log(WLR_INFO, "Contents of %s:", paths[i]);
			while (!feof(f)) {
				char *line;
				if (!(line = read_line(f))) {
					break;
				}
				if (*line) {
					wlr_log(WLR_INFO, "%s", line);
				}
				free(line);
			}
			fclose(f);
		}
	}
}

static void log_kernel(void) {
	FILE *f = popen("uname -a", "r");
	if (!f) {
		wlr_log(WLR_INFO, "Unable to determine kernel version");
		return;
	}
	while (!feof(f)) {
		char *line;
		if (!(line = read_line(f))) {
			break;
		}
		if (*line) {
			wlr_log(WLR_INFO, "%s", line);
		}
		free(line);
	}
	pclose(f);
}


static bool drop_permissions(void) {
	if (getuid() != geteuid() || getgid() != getegid()) {
		if (setgid(getgid()) != 0) {
			wlr_log(WLR_ERROR, "Unable to drop root, refusing to start");
			return false;
		}
		if (setuid(getuid()) != 0) {
			wlr_log(WLR_ERROR, "Unable to drop root, refusing to start");
			return false;
		}
	}
	if (setuid(0) != -1) {
		wlr_log(WLR_ERROR, "Unable to drop root (we shouldn't be able to "
			"restore it after setuid), refusing to start");
		return false;
	}
	return true;
}

void enable_debug_flag(const char *flag) {
	if (strcmp(flag, "damage=highlight") == 0) {
		debug.damage = DAMAGE_HIGHLIGHT;
	} else if (strcmp(flag, "damage=rerender") == 0) {
		debug.damage = DAMAGE_RERENDER;
	} else if (strcmp(flag, "noatomic") == 0) {
		debug.noatomic = true;
	} else if (strcmp(flag, "render-tree") == 0) {
		debug.render_tree = true;
	} else if (strcmp(flag, "txn-wait") == 0) {
		debug.txn_wait = true;
	} else if (strcmp(flag, "txn-timings") == 0) {
		debug.txn_timings = true;
	} else if (strncmp(flag, "txn-timeout=", 12) == 0) {
		server.txn_timeout_ms = atoi(&flag[12]);
	}
}

int main(int argc, char **argv) {
	static int verbose = 0, debug = 0, validate = 0, allow_unsupported_gpu = 0;

	static struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"config", required_argument, NULL, 'c'},
		{"validate", no_argument, NULL, 'C'},
		{"debug", no_argument, NULL, 'd'},
		{"version", no_argument, NULL, 'v'},
		{"verbose", no_argument, NULL, 'V'},
		{"get-socketpath", no_argument, NULL, 'p'},
		{"unsupported-gpu", no_argument, NULL, 'u'},
		{"my-next-gpu-wont-be-nvidia", no_argument, NULL, 'u'},
		{0, 0, 0, 0}
	};

	char *config_path = NULL;

	const char* usage =
		"Usage: sway [options] [command]\n"
		"\n"
		"  -h, --help             Show help message and quit.\n"
		"  -c, --config <config>  Specify a config file.\n"
		"  -C, --validate         Check the validity of the config file, then exit.\n"
		"  -d, --debug            Enables full logging, including debug information.\n"
		"  -v, --version          Show the version number and quit.\n"
		"  -V, --verbose          Enables more verbose logging.\n"
		"      --get-socketpath   Gets the IPC socket path and prints it, then exits.\n"
		"\n";

	int c;
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "hCdD:vVc:", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'h': // help
			fprintf(stdout, "%s", usage);
			exit(EXIT_SUCCESS);
			break;
		case 'c': // config
			config_path = strdup(optarg);
			break;
		case 'C': // validate
			validate = 1;
			break;
		case 'd': // debug
			debug = 1;
			break;
		case 'D': // extended debug options
			enable_debug_flag(optarg);
			break;
		case 'u':
			allow_unsupported_gpu = 1;
			break;
		case 'v': // version
			fprintf(stdout, "sway version " SWAY_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		case 'V': // verbose
			verbose = 1;
			break;
		case 'p': ; // --get-socketpath
			if (getenv("SWAYSOCK")) {
				fprintf(stdout, "%s\n", getenv("SWAYSOCK"));
				exit(EXIT_SUCCESS);
			} else {
				fprintf(stderr, "sway socket not detected.\n");
				exit(EXIT_FAILURE);
			}
			break;
		default:
			fprintf(stderr, "%s", usage);
			exit(EXIT_FAILURE);
		}
	}

	if (debug) {
		wlr_log_init(WLR_DEBUG, NULL);
	} else if (verbose || validate) {
		wlr_log_init(WLR_INFO, NULL);
	} else {
		wlr_log_init(WLR_ERROR, NULL);
	}

	if (optind < argc) { // Behave as IPC client
		if (optind != 1) {
			wlr_log(WLR_ERROR, "Don't use options with the IPC client");
			exit(EXIT_FAILURE);
		}
		if (!drop_permissions()) {
			exit(EXIT_FAILURE);
		}
		char *socket_path = getenv("SWAYSOCK");
		if (!socket_path) {
			wlr_log(WLR_ERROR, "Unable to retrieve socket path");
			exit(EXIT_FAILURE);
		}
		char *command = join_args(argv + optind, argc - optind);
		run_as_ipc_client(command, socket_path);
		return 0;
	}

	if (!server_privileged_prepare(&server)) {
		return 1;
	}

	log_kernel();
	log_distro();
	detect_proprietary(allow_unsupported_gpu);
	detect_raspi();

	if (!drop_permissions()) {
		server_fini(&server);
		exit(EXIT_FAILURE);
	}

	// handle SIGTERM signals
	signal(SIGTERM, sig_handler);

	// prevent ipc from crashing sway
	signal(SIGPIPE, SIG_IGN);

	wlr_log(WLR_INFO, "Starting sway version " SWAY_VERSION);

	root = root_create();

	if (!server_init(&server)) {
		return 1;
	}

	ipc_init(&server);
	log_env();

	if (validate) {
		bool valid = load_main_config(config_path, false, true);
		return valid ? 0 : 1;
	}

	setenv("WAYLAND_DISPLAY", server.socket, true);
	if (!load_main_config(config_path, false, false)) {
		sway_terminate(EXIT_FAILURE);
	}

	if (config_path) {
		free(config_path);
	}

	if (!terminate_request) {
		if (!server_start_backend(&server)) {
			sway_terminate(EXIT_FAILURE);
		}
	}

	config->active = true;
	load_swaybars();
	// Execute commands until there are none left
	wlr_log(WLR_DEBUG, "Running deferred commands");
	while (config->cmd_queue->length) {
		char *line = config->cmd_queue->items[0];
		list_t *res_list = execute_command(line, NULL, NULL);
		for (int i = 0; i < res_list->length; ++i) {
			struct cmd_results *res = res_list->items[i];
			if (res->status != CMD_SUCCESS) {
				wlr_log(WLR_ERROR, "Error on line '%s': %s", line, res->error);
			}
			free_cmd_results(res);
		}
		list_free(res_list);
		free(line);
		list_del(config->cmd_queue, 0);
	}
	transaction_commit_dirty();

	if (config->swaynag_config_errors.pid > 0) {
		swaynag_show(&config->swaynag_config_errors);
	}

	if (!terminate_request) {
		server_run(&server);
	}

	wlr_log(WLR_INFO, "Shutting down sway");

	server_fini(&server);
	root_destroy(root);
	root = NULL;

	if (config) {
		free_config(config);
	}

	pango_cairo_font_map_set_default(NULL);

	return exit_value;
}
