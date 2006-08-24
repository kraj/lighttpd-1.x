#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "log.h"
#include "buffer.h"

#include "plugin.h"

#include "response.h"
#include "stat_cache.h"

/**
 * this is a uploadprogress for a lighttpd plugin
 *
 */

typedef struct {
	buffer     *con_id;
	connection *con;
} connection_map_entry;

typedef struct {
	connection_map_entry **ptr;

	size_t used;
	size_t size;
} connection_map;

/* plugin config for all request/connections */

typedef struct {
	buffer *progress_url;
} plugin_config;

typedef struct {
	PLUGIN_DATA;

	connection_map *con_map;

	plugin_config **config_storage;

	plugin_config conf;
} plugin_data;

/**
 *
 * connection maps
 *
 */

/* init the plugin data */
connection_map *connection_map_init() {
	connection_map *cm;

	cm = calloc(1, sizeof(*cm));

	return cm;
}

void connection_map_free(connection_map *cm) {
	size_t i;
	for (i = 0; i < cm->size; i++) {
		connection_map_entry *cme = cm->ptr[i];

		if (!cme) break;

		if (cme->con_id) {
			buffer_free(cme->con_id);
		}
		free(cme);
	}

	free(cm);
}

int connection_map_insert(connection_map *cm, connection *con, buffer *con_id) {
	connection_map_entry *cme;
	size_t i;

	if (cm->size == 0) {
		cm->size = 16;
		cm->ptr = malloc(cm->size * sizeof(*(cm->ptr)));
		for (i = 0; i < cm->size; i++) {
			cm->ptr[i] = NULL;
		}
	} else if (cm->used == cm->size) {
		cm->size += 16;
		cm->ptr = realloc(cm->ptr, cm->size * sizeof(*(cm->ptr)));
		for (i = cm->used; i < cm->size; i++) {
			cm->ptr[i] = NULL;
		}
	}

	if (cm->ptr[cm->used]) {
		/* is already alloced, just reuse it */
		cme = cm->ptr[cm->used];
	} else {
		cme = malloc(sizeof(*cme));
	}
	cme->con_id = buffer_init();
	buffer_copy_string_buffer(cme->con_id, con_id);
	cme->con = con;

	cm->ptr[cm->used++] = cme;

	return 0;
}

connection *connection_map_get_connection(connection_map *cm, buffer *con_id) {
	size_t i;

	for (i = 0; i < cm->used; i++) {
		connection_map_entry *cme = cm->ptr[i];

		if (buffer_is_equal(cme->con_id, con_id)) {
			/* found connection */

			return cme->con;
		}
	}
	return NULL;
}

int connection_map_remove_connection(connection_map *cm, connection *con) {
	size_t i;

	for (i = 0; i < cm->used; i++) {
		connection_map_entry *cme = cm->ptr[i];

		if (cme->con == con) {
			/* found connection */

			buffer_reset(cme->con_id);
			cme->con = NULL;

			cm->used--;

			/* swap positions with the last entry */
			if (cm->used) {
				cm->ptr[i] = cm->ptr[cm->used];
				cm->ptr[cm->used] = cme;
			}

			return 1;
		}
	}

	return 0;
}

/* init the plugin data */
INIT_FUNC(mod_uploadprogress_init) {
	plugin_data *p;

	p = calloc(1, sizeof(*p));

	p->con_map = connection_map_init();

	return p;
}

/* detroy the plugin data */
FREE_FUNC(mod_uploadprogress_free) {
	plugin_data *p = p_d;

	UNUSED(srv);

	if (!p) return HANDLER_GO_ON;

	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];

			buffer_free(s->progress_url);

			free(s);
		}
		free(p->config_storage);
	}

	connection_map_free(p->con_map);

	free(p);

	return HANDLER_GO_ON;
}

/* handle plugin config and check values */

SETDEFAULTS_FUNC(mod_uploadprogress_set_defaults) {
	plugin_data *p = p_d;
	size_t i = 0;

	config_values_t cv[] = {
		{ "upload-progress.progress-url", NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ NULL,                         NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};

	if (!p) return HANDLER_ERROR;

	p->config_storage = calloc(1, srv->config_context->used * sizeof(specific_config *));

	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;

		s = calloc(1, sizeof(plugin_config));
		s->progress_url    = buffer_init();

		cv[0].destination = s->progress_url;

		p->config_storage[i] = s;

		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
			return HANDLER_ERROR;
		}
	}

	return HANDLER_GO_ON;
}

static int mod_uploadprogress_patch_connection(server *srv, connection *con, plugin_data *p) {
	size_t i, j;
	plugin_config *s = p->config_storage[0];

	PATCH_OPTION(progress_url);

	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		s = p->config_storage[i];

		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;

		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];

			if (buffer_is_equal_string(du->key, CONST_STR_LEN("upload-progress.progress-url"))) {
				PATCH_OPTION(progress_url);
			}
		}
	}

	return 0;
}

/**
 *
 * the idea:
 *
 * for the first request we check if it is a post-request
 *
 * if no, move out, don't care about them
 *
 * if yes, take the connection structure and register it locally
 * in the progress-struct together with an session-id (md5 ... )
 *
 * if the connections closes, cleanup the entry in the progress-struct
 *
 * a second request can now get the info about the size of the upload,
 * the received bytes
 *
 */

URIHANDLER_FUNC(mod_uploadprogress_uri_handler) {
	plugin_data *p = p_d;
	size_t i;
	data_string *ds;
	buffer *b, *tracking_id;
	connection *post_con = NULL;

	UNUSED(srv);

	if (con->uri.path->used == 0) return HANDLER_GO_ON;

	mod_uploadprogress_patch_connection(srv, con, p);

	/* check if this is a POST request */
	switch(con->request.http_method) {
	case HTTP_METHOD_POST:
		/* the request has to contain a 32byte ID */

		if (NULL == (ds = (data_string *)array_get_element(con->request.headers, "X-Progress-ID"))) {
			if (!buffer_is_empty(con->uri.query)) {
				/* perhaps the POST request is using the querystring to pass the X-Progress-ID */
				b = con->uri.query;
			} else {
				return HANDLER_GO_ON;
			}
		} else {
			b = ds->value;
		}

		if (b->used != 32 + 1) {
			ERROR("the Progress-ID has to be 32 characters long, got %d characters", b->used - 1);

			return HANDLER_GO_ON;
		}

		for (i = 0; i < b->used - 1; i++) {
			char c = b->ptr[i];

			if (!light_isxdigit(c)) {
				ERROR("only hex-digits are allowed (0-9 + a-f): (ascii: %d)", c);

				return HANDLER_GO_ON;
			}
		}

		connection_map_insert(p->con_map, con, b);

		return HANDLER_GO_ON;
	case HTTP_METHOD_GET:
		if (!buffer_is_equal(con->uri.path, p->conf.progress_url)) {
			return HANDLER_GO_ON;
		}

		if (NULL == (ds = (data_string *)array_get_element(con->request.headers, "X-Progress-ID"))) {
			if (!buffer_is_empty(con->uri.query)) {
				/* perhaps the GET request is using the querystring to pass the X-Progress-ID */
				tracking_id = con->uri.query;
			} else {
				return HANDLER_GO_ON;
			}
		} else {
			tracking_id = ds->value;
		}

		if (tracking_id->used != 32 + 1) {
			ERROR("the Progress-ID has to be 32 characters long, got %d characters", tracking_id->used - 1);

			return HANDLER_GO_ON;
		}

		for (i = 0; i < tracking_id->used - 1; i++) {
			char c = tracking_id->ptr[i];

			if (!light_isxdigit(c)) {
				ERROR("only hex-digits are allowed (0-9 + a-f): (ascii: %d)", c);

				return HANDLER_GO_ON;
			}
		}

		buffer_reset(con->physical.path);

		con->file_started = 1;
		con->http_status = 200;
		con->send->is_closed = 1;

		/* send JSON content */

		response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/javascript"));

		/* just an attempt the force the IE/proxies to NOT cache the request */
		response_header_overwrite(srv, con, CONST_STR_LEN("Pragma"), CONST_STR_LEN("no-cache"));
		response_header_overwrite(srv, con, CONST_STR_LEN("Expires"), CONST_STR_LEN("Thu, 19 Nov 1981 08:52:00 GMT"));
		response_header_overwrite(srv, con, CONST_STR_LEN("Cache-Control"), 
				CONST_STR_LEN("no-store, no-cache, must-revalidate, post-check=0, pre-check=0"));

		b = chunkqueue_get_append_buffer(con->send);

		/* get the connection */
		if (NULL == (post_con = connection_map_get_connection(p->con_map, tracking_id))) {
			BUFFER_APPEND_STRING_CONST(b, "new Object({ 'status' : 'starting' })\r\n");

			return HANDLER_FINISHED;
		}

		/* prepare XML */
		BUFFER_COPY_STRING_CONST(b, "new Object({ 'state' : ");
		buffer_append_string(b, post_con->recv->is_closed ? "'done'" : "'uploading'");
		BUFFER_APPEND_STRING_CONST(b, ", 'size' : ");
		buffer_append_off_t(b, post_con->request.content_length == -1 ? 0 : post_con->request.content_length);
		BUFFER_APPEND_STRING_CONST(b, ", 'received' : ");
		buffer_append_off_t(b, post_con->recv->bytes_in);
		BUFFER_APPEND_STRING_CONST(b, "})\r\n");

		return HANDLER_FINISHED;
	default:
		break;
	}

	return HANDLER_GO_ON;
}

REQUESTDONE_FUNC(mod_uploadprogress_request_done) {
	plugin_data *p = p_d;

	UNUSED(srv);

	if (con->uri.path->used == 0) return HANDLER_GO_ON;

	if (connection_map_remove_connection(p->con_map, con)) {
		/* removed */
	}

	return HANDLER_GO_ON;
}

/* this function is called at dlopen() time and inits the callbacks */

int mod_uploadprogress_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("uploadprogress");

	p->init        = mod_uploadprogress_init;
	p->handle_uri_clean = mod_uploadprogress_uri_handler;
	p->handle_response_done  = mod_uploadprogress_request_done;
	p->set_defaults  = mod_uploadprogress_set_defaults;
	p->cleanup     = mod_uploadprogress_free;

	p->data        = NULL;

	return 0;
}
