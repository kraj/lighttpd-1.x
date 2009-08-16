
#include "sigchild_handler.h"
#include "splaytree.h"
#include "fdevent.h"
#include "log.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

struct sc_data;
typedef struct sc_data sc_data;
struct sc_data {
	sc_handler handler;
	void *data;
};

static splay_tree *st_sigchild;

int sigchild_pipe[2];

iosocket *sigchild_read;

int sigchild_register(pid_t pid, sc_handler handler, void *data) {
	sc_data *d = malloc(sizeof(*d));
	d->handler = handler;
	d->data = data;
	st_sigchild = splaytree_insert(st_sigchild, pid, d);
	return 0;
}

void sigchild_unregister(pid_t pid) {
	st_sigchild = splaytree_splay(st_sigchild, pid);
	if (st_sigchild && st_sigchild->key == pid) {
		free(st_sigchild->data);
		st_sigchild = splaytree_delete(st_sigchild, pid);
	}
}

void sigchild_handler(server *srv) {
	char buf[32];
	int st;
	pid_t pid;

	TRACE("%s", "SIGCHILD");
	while (read(sigchild_pipe[0], buf, 32) > 0) ;
	for (;;) {
		pid = waitpid((pid_t) -1, &st, WNOHANG);
		if (pid == 0) break;
		if (pid == -1) {
			if (errno == EINTR) continue;
			break;
		}
		st_sigchild = splaytree_splay(st_sigchild, pid);
		if (st_sigchild && st_sigchild->key == pid) {
			sc_data *data = (sc_data*) st_sigchild->data;
			st_sigchild = splaytree_delete(st_sigchild, pid);
			data->handler(srv, pid, st, data->data);
			free(data);
		}
	}
}
