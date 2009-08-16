#ifndef _SIGCHILD_HANDLER_H_
#define _SIGCHILD_HANDLER_H_

#include "base.h"

#include <unistd.h>

typedef void (*sc_handler)(server *srv, pid_t pid, int s, void *data);

int sigchild_register(pid_t pid, sc_handler handler, void *data);
void sigchild_unregister(pid_t pid);
void sigchild_handler(server *srv);

#endif
