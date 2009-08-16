#include "mod_proxy_core_backlog.h"
#include "array-static.h"

typedef struct {
	connection *con;
	time_t added_ts; /* when was the entry added (for timeout handling) */
} proxy_request;

static proxy_request *proxy_request_init(connection *con, time_t cur_ts) {
	proxy_request *req = g_slice_new0(proxy_request);
	req->con = con;
	req->added_ts = cur_ts;

	return req;
}

static void proxy_request_free(proxy_request *req) {
	g_slice_free(proxy_request, req);
}

proxy_backlog *proxy_backlog_init(void) {
	proxy_backlog *backlog = g_slice_new0(proxy_backlog);
	g_queue_init(&backlog->queue);

	return backlog;
}

void proxy_backlog_free(proxy_backlog *backlog) {
	proxy_request *req;
	if (!backlog) return;

	while (NULL != (req = (proxy_request*) g_queue_pop_head(&backlog->queue))) {
		proxy_request_free(req);
	}

	g_slice_free(proxy_backlog, backlog);
}

int proxy_backlog_push(proxy_backlog *backlog, connection *con, time_t cur_ts) {
	g_queue_push_tail(&backlog->queue, proxy_request_init(con, cur_ts));

	return 0;
}

/**
 * remove the first element from the backlog
 */
connection *proxy_backlog_shift(proxy_backlog *backlog) {
	proxy_request *req = (proxy_request*) g_queue_pop_head(&backlog->queue);
	connection *con = NULL;

	if (req) {
		con = req->con;
		proxy_request_free(req);
	}

	return con;
}

static gint proxy_backlog_find_connection(gconstpointer el, gconstpointer con) {
	return ( ((const proxy_request*) el)->con == (const connection*) con ) ? 0 : 1;
}

int proxy_backlog_remove_connection(proxy_backlog *backlog, connection *con) {
	GList *el;

	if (!con) return -1;

	el = g_queue_find_custom(&backlog->queue, con, proxy_backlog_find_connection);

	if (!el) return -1;

	proxy_request_free((proxy_request*) el->data);
	g_queue_delete_link(&backlog->queue, el);

	return 0;
}
