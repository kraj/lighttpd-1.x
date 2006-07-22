#ifndef _MOD_PROXY_CORE_H_
#define _MOD_PROXY_CORE_H_

#include "buffer.h"
#include "plugin.h"
#include "http_resp.h"
#include "array.h"

#include "mod_proxy_core_pool.h"	
#include "mod_proxy_core_backend.h"
#include "mod_proxy_core_backlog.h"
#include "mod_proxy_core_rewrites.h"

typedef struct {
	proxy_backends *backends;

	proxy_backlog *backlog;

	proxy_rewrites *request_rewrites;
	proxy_rewrites *response_rewrites;

	unsigned short allow_x_sendfile;
	unsigned short debug;

	proxy_balance_t balancer;
	proxy_protocol_t protocol;
} plugin_config;

typedef struct {
	PLUGIN_DATA;

	http_resp *resp;

	array *possible_balancers;
	array *possible_protocols;

	/* for parsing only */
	array *backends_arr;
	buffer *protocol_buf;
	buffer *balance_buf;

	buffer *replace_buf;

	plugin_config **config_storage;

	plugin_config conf;
} plugin_data;


typedef enum {
	PROXY_STATE_UNSET,
	PROXY_STATE_CONNECTING,
	PROXY_STATE_CONNECTED,
	PROXY_STATE_WRITE_REQUEST_HEADER,
	PROXY_STATE_WRITE_REQUEST_BODY,
	PROXY_STATE_READ_RESPONSE_HEADER,
	PROXY_STATE_READ_RESPONSE_BODY
} proxy_state_t;

typedef struct {
	proxy_connection *proxy_con;
	proxy_backend *proxy_backend;

	connection *remote_con;

	array *request_headers;

	int is_chunked;            /** is the incoming content chunked (for HTTP) */
	int send_response_content; /** 0 if we have to ignore the content-body */
	int send_static_file;      /** 1 if we do a internal redirect to the ->mode = DIRECT */
	
	/**
	 * chunkqueues
	 * - the encoded_rb is the raw network stuff
	 * - the rb is filtered through the stream decoder
	 *
	 * - wb is the normal bytes stream
	 * - encoded_wb is encoded for the network by the stream encoder
	 */
	chunkqueue *recv;
	chunkqueue *recv_raw;
	chunkqueue *send_raw;
	chunkqueue *send;
	
	off_t bytes_read;
	off_t content_length;

	proxy_state_t state;
} proxy_session;

#endif
