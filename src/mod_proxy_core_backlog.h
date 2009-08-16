#ifndef _MOD_PROXY_CORE_BACKLOG_H_
#define _MOD_PROXY_CORE_BACKLOG_H_

#include "base.h"

#include <glib.h>

/**
 * a we can't get a connection from the pool, queue the request in the
 * request queue (FIFO)
 *
 * - the queue is infinite
 * - entries are removed after a timeout (status 504)
 */
typedef struct {
	GQueue queue;
} proxy_backlog;

proxy_backlog *proxy_backlog_init(void);
void proxy_backlog_free(proxy_backlog *backlog);

/**
 * append a request to the end
 *
 * @return 0 in success, -1 if full
 */
int proxy_backlog_push(proxy_backlog *backlog, connection *con, time_t cur_ts);

/**
 * remove the first request from the backlog
 *
 * @return NULL if backlog is empty, the request otherwise
 */
connection *proxy_backlog_shift(proxy_backlog *backlog);

/**
 * remove the request with the connection 'con' from the backlog
 *
 * @return -1 if not found, 0 otherwise
 */
int proxy_backlog_remove_connection(proxy_backlog *backlog, connection *con);

#endif
