#ifndef _SWAYBAR_IPC_H
#define _SWAYBAR_IPC_H
#include <stdbool.h>
#include "swaybar/bar.h"

bool ipc_initialize(struct swaybar *bar, const char *bar_id);
bool handle_ipc_readable(struct swaybar *bar);
void ipc_get_workspaces(struct swaybar *bar);
void ipc_send_workspace_command(struct swaybar *bar, const char *ws);

#endif
