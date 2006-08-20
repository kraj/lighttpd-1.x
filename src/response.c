#include <sys/types.h>
#include <sys/stat.h>

#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <assert.h>

#include <stdio.h>

#include "response.h"
#include "keyvalue.h"
#include "log.h"
#include "stat_cache.h"
#include "chunk.h"

#include "connections.h"

#include "plugin.h"

#include "sys-socket.h"
#include "sys-files.h"
#include "sys-strings.h"

int http_response_write_header(server *srv, connection *con, chunkqueue *raw) {
	buffer *b;
	size_t i;
	int have_date = 0;
	int have_server = 0;

	b = chunkqueue_get_prepend_buffer(raw);

	if (con->request.http_version == HTTP_VERSION_1_1) {
		BUFFER_COPY_STRING_CONST(b, "HTTP/1.1 ");
	} else {
		BUFFER_COPY_STRING_CONST(b, "HTTP/1.0 ");
	}
	buffer_append_long(b, con->http_status);
	BUFFER_APPEND_STRING_CONST(b, " ");
	buffer_append_string(b, get_http_status_name(con->http_status));

	if (con->request.http_version != HTTP_VERSION_1_1 || con->keep_alive == 0) {
		BUFFER_APPEND_STRING_CONST(b, "\r\nConnection: ");
		buffer_append_string(b, con->keep_alive ? "keep-alive" : "close");
	}

	if (con->response.transfer_encoding & HTTP_TRANSFER_ENCODING_CHUNKED) {
		BUFFER_APPEND_STRING_CONST(b, "\r\nTransfer-Encoding: chunked");
	}


	/* add all headers */
	for (i = 0; i < con->response.headers->used; i++) {
		data_string *ds;

		ds = (data_string *)con->response.headers->data[i];

		if (ds->value->used && ds->key->used &&
		    0 != strncmp(ds->key->ptr, "X-LIGHTTPD-", sizeof("X-LIGHTTPD-") - 1) &&
		    0 != strcasecmp(ds->key->ptr, "X-Sendfile")) {
			if (buffer_is_equal_string(ds->key, CONST_STR_LEN("Date"))) have_date = 1;
			if (buffer_is_equal_string(ds->key, CONST_STR_LEN("Server"))) have_server = 1;

			BUFFER_APPEND_STRING_CONST(b, "\r\n");
			buffer_append_string_buffer(b, ds->key);
			BUFFER_APPEND_STRING_CONST(b, ": ");
			buffer_append_string_buffer(b, ds->value);
#if 0
			log_error_write(srv, __FILE__, __LINE__, "bb",
					ds->key, ds->value);
#endif
		}
	}

	if (!have_date) {
		/* HTTP/1.1 requires a Date: header */
		BUFFER_APPEND_STRING_CONST(b, "\r\nDate: ");

		/* cache the generated timestamp */
		if (srv->cur_ts != srv->last_generated_date_ts) {
			buffer_prepare_copy(srv->ts_date_str, 255);

			strftime(srv->ts_date_str->ptr, srv->ts_date_str->size - 1,
				 "%a, %d %b %Y %H:%M:%S GMT", gmtime(&(srv->cur_ts)));

			srv->ts_date_str->used = strlen(srv->ts_date_str->ptr) + 1;

			srv->last_generated_date_ts = srv->cur_ts;
		}

		buffer_append_string_buffer(b, srv->ts_date_str);
	}

	if (!have_server) {
		if (buffer_is_empty(con->conf.server_tag)) {
			BUFFER_APPEND_STRING_CONST(b, "\r\nServer: " PACKAGE_NAME "/" PACKAGE_VERSION);
		} else {
			BUFFER_APPEND_STRING_CONST(b, "\r\nServer: ");
			buffer_append_string_buffer(b, con->conf.server_tag);
		}
	}

	BUFFER_APPEND_STRING_CONST(b, "\r\n\r\n");


	con->bytes_header = b->used - 1;

	if (con->conf.log_response_header) {
		log_error_write(srv, __FILE__, __LINE__, "sSb", "Response-Header:", "\n", b);
	}

	return 0;
}



handler_t handle_get_backend(server *srv, connection *con) {
	handler_t r;

	/* looks like someone has already made a decision */
	if (con->mode == DIRECT &&
	    (con->http_status != 0 && con->http_status != 200)) {
		/* remove a packets in the queue */

		return HANDLER_FINISHED;
	}

	/* no decision yet, build conf->filename */
	if (con->mode == DIRECT && con->physical.path->used == 0) {
		char *qstr;

		/* we only come here when we have to parse the full request again
		 *
		 * a HANDLER_COMEBACK from mod_rewrite and mod_fastcgi might be a
		 * problem here as mod_setenv might get called multiple times
		 *
		 * fastcgi-auth might lead to a COMEBACK too
		 * fastcgi again dead server too
		 *
		 * mod_compress might add headers twice too
		 *
		 *  */

		if (con->conf.log_condition_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "run condition");
		}
		config_patch_connection(srv, con, COMP_SERVER_SOCKET); /* SERVERsocket */

		/**
		 * prepare strings
		 *
		 * - uri.path_raw
		 * - uri.path (secure)
		 * - uri.query
		 *
		 */

		/**
		 * Name according to RFC 2396
		 *
		 * - scheme
		 * - authority
		 * - path
		 * - query
		 *
		 * (scheme)://(authority)(path)?(query)
		 *
		 *
		 */

		buffer_copy_string(con->uri.scheme, con->conf.is_ssl ? "https" : "http");
		buffer_copy_string_buffer(con->uri.authority, con->request.http_host);
		buffer_to_lower(con->uri.authority);

		config_patch_connection(srv, con, COMP_HTTP_HOST);      /* Host:        */
		config_patch_connection(srv, con, COMP_HTTP_REMOTEIP);  /* Client-IP */
		config_patch_connection(srv, con, COMP_HTTP_REFERER);   /* Referer:     */
		config_patch_connection(srv, con, COMP_HTTP_USERAGENT); /* User-Agent:  */
		config_patch_connection(srv, con, COMP_HTTP_COOKIE);    /* Cookie:  */

		/** extract query string from request.uri */
		if (NULL != (qstr = strchr(con->request.uri->ptr, '?'))) {
			buffer_copy_string    (con->uri.query, qstr + 1);
			buffer_copy_string_len(con->uri.path_raw, con->request.uri->ptr, qstr - con->request.uri->ptr);
		} else {
			buffer_reset     (con->uri.query);
			buffer_copy_string_buffer(con->uri.path_raw, con->request.uri);
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- splitting Request-URI");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Request-URI  : ", con->request.uri);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-scheme   : ", con->uri.scheme);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-authority: ", con->uri.authority);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-path     : ", con->uri.path_raw);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-query    : ", con->uri.query);
		}

		/* disable keep-alive if requested */

		if (con->request_count > con->conf.max_keep_alive_requests) {
			con->keep_alive = 0;
		}


		/**
		 *
		 * call plugins
		 *
		 * - based on the raw URL
		 *
		 */

		switch(r = plugins_call_handle_uri_raw(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sd", "handle_uri_raw: unknown return value", r);
			break;
		}

		/* build filename
		 *
		 * - decode url-encodings  (e.g. %20 -> ' ')
		 * - remove path-modifiers (e.g. /../)
		 */



		if (con->request.http_method == HTTP_METHOD_OPTIONS &&
		    con->uri.path_raw->ptr[0] == '*' && con->uri.path_raw->ptr[1] == '\0') {
			/* OPTIONS * ... */
			buffer_copy_string_buffer(con->uri.path, con->uri.path_raw);
		} else {
			buffer_copy_string_buffer(srv->tmp_buf, con->uri.path_raw);
			buffer_urldecode_path(srv->tmp_buf);
			buffer_path_simplify(con->uri.path, srv->tmp_buf);
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- sanatising URI");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "URI-path     : ", con->uri.path);
		}

		/**
		 *
		 * call plugins
		 *
		 * - based on the clean URL
		 *
		 */

		config_patch_connection(srv, con, COMP_HTTP_URL); /* HTTPurl */
		config_patch_connection(srv, con, COMP_HTTP_QUERYSTRING); /* HTTPqs */

		/* do we have to downgrade to 1.0 ? */
		if (!con->conf.allow_http11) {
			con->request.http_version = HTTP_VERSION_1_0;
		}

		switch(r = plugins_call_handle_uri_clean(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "");
			break;
		}

		if (con->request.http_method == HTTP_METHOD_OPTIONS &&
		    con->uri.path->ptr[0] == '*' && con->uri.path_raw->ptr[1] == '\0') {
			/* option requests are handled directly without checking the path */

			response_header_insert(srv, con, CONST_STR_LEN("Allow"), CONST_STR_LEN("OPTIONS, GET, HEAD, POST"));

			con->http_status = 200;
			/* no more content to send */
			con->send->is_closed = 1;

			return HANDLER_FINISHED;
		}

		/***
		 *
		 * border
		 *
		 * logical filename (URI) becomes a physical filename here
		 *
		 *
		 *
		 */




		/* 1. stat()
		 * ... ISREG() -> ok, go on
		 * ... ISDIR() -> index-file -> redirect
		 *
		 * 2. pathinfo()
		 * ... ISREG()
		 *
		 * 3. -> 404
		 *
		 */

		/*
		 * SEARCH DOCUMENT ROOT
		 */

		/* set a default */

		buffer_copy_string_buffer(con->physical.doc_root, con->conf.document_root);
		buffer_copy_string_buffer(con->physical.rel_path, con->uri.path);

		filename_unix2local(con->physical.rel_path);
#if defined(_WIN32) || defined(__CYGWIN__)
		/* strip dots and spaces from the end
		 *
		 * windows/dos handle those filenames as the same file
		 *
		 * foo == foo. == foo..... == "foo...   " == "foo..  ./"
		 *
		 * This will affect PATHINFO in some cases
		 *
		 * on native windows we could prepend the filename with \\?\ to circumvent
		 * this behaviour. I have no idea how to push this through cygwin
		 *
		 * */

		if (con->physical.rel_path->used > 1) {
			buffer *b = con->physical.rel_path;
			size_t i;

			if (b->used > 2 &&
			    b->ptr[b->used-2] == '/' &&
			    (b->ptr[b->used-3] == ' ' ||
			     b->ptr[b->used-3] == '.')) {
				b->ptr[b->used--] = '\0';
			}

			for (i = b->used - 2; b->used > 1; i--) {
				if (b->ptr[i] == ' ' ||
				    b->ptr[i] == '.') {
					b->ptr[b->used--] = '\0';
				} else {
					break;
				}
			}
		}
#endif

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- before doc_root");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Doc-Root     :", con->physical.doc_root);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Rel-Path     :", con->physical.rel_path);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}
		/* the docroot plugin should set the doc_root and might also set the physical.path
		 * for us (all vhost-plugins are supposed to set the doc_root)
		 * */
		switch(r = plugins_call_handle_docroot(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "");
			break;
		}

		/* The default Mac OS X and Windows filesystems can't distiguish between
		 * upper- and lowercase, so convert to lowercase
		 */
		if (con->conf.force_lowercase_filenames) {
			buffer_to_lower(con->physical.rel_path);
		}

		/* the docroot plugins might set the servername; if they don't we take http-host */
		if (buffer_is_empty(con->server_name)) {
			buffer_copy_string_buffer(con->server_name, con->uri.authority);
		}

		/**
		 * create physical filename
		 * -> physical.path = docroot + rel_path
		 *
		 */

		buffer_copy_string_buffer(con->physical.path, con->physical.doc_root);
		PATHNAME_APPEND_SLASH(con->physical.path);
		buffer_copy_string_buffer(con->physical.basedir, con->physical.path);
		if (con->physical.rel_path->used &&
		    con->physical.rel_path->ptr[0] == DIR_SEPERATOR) {
			buffer_append_string_len(con->physical.path, con->physical.rel_path->ptr + 1, con->physical.rel_path->used - 2);
		} else {
			buffer_append_string_buffer(con->physical.path, con->physical.rel_path);
		}

		/* win32: directories can't have a trailing slash */
		if (con->physical.path->ptr[con->physical.path->used - 2] == DIR_SEPERATOR) {
			con->physical.path->ptr[con->physical.path->used - 2] = '\0';
			con->physical.path->used--;
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- after doc_root");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Doc-Root     :", con->physical.doc_root);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Rel-Path     :", con->physical.rel_path);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}

		switch(r = plugins_call_handle_physical(srv, con)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_FINISHED:
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "");
			break;
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- logical -> physical");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Doc-Root     :", con->physical.doc_root);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Rel-Path     :", con->physical.rel_path);
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}
	}

	/*
	 * No one took the file away from the normal path of execution yet (like mod_access)
	 *
	 * we don't have a backend yet, try to resolve the physical path and go on
	 * 
	 */

	if (con->mode == DIRECT) {
		char *slash = NULL;
		char *pathinfo = NULL;
		int found = 0;
		stat_cache_entry *sce = NULL;

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- handling physical path");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}

		if (HANDLER_ERROR != stat_cache_get_entry(srv, con, con->physical.path, &sce)) {
			/* file exists */

			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__,  "s",  "-- file found");
				log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
			}

#ifdef HAVE_LSTAT
			if ((sce->is_symlink != 0) && !con->conf.follow_symlink) {
				con->http_status = 403;

				if (con->conf.log_request_handling) {
					log_error_write(srv, __FILE__, __LINE__,  "s",  "-- access denied due symlink restriction");
					log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				}

				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			};
#endif

			if (S_ISDIR(sce->st.st_mode)) {
				if (con->uri.path->ptr[con->uri.path->used - 2] != '/') {
					/* redirect to .../ */

					http_response_redirect_to_directory(srv, con);

					return HANDLER_FINISHED;
				}
#ifdef HAVE_LSTAT
			} else if (!S_ISREG(sce->st.st_mode) && !sce->is_symlink) {
#else
			} else if (!S_ISREG(sce->st.st_mode)) {
#endif
				/* any special handling of non-reg files ?*/


			}
		} else {
			switch (errno) {
			case EACCES:
				con->http_status = 403;

				if (con->conf.log_request_handling) {
					log_error_write(srv, __FILE__, __LINE__,  "s",  "-- access denied");
					log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				}

				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			case ENOENT:
				con->http_status = 404;

				if (con->conf.log_request_handling) {
					log_error_write(srv, __FILE__, __LINE__,  "s",  "-- file not found");
					log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				}

				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			case ENOTDIR:
				/* PATH_INFO ! :) */
				break;
			default:
				/* we have no idea what happened, so tell the user. */
				con->http_status = 500;
				buffer_reset(con->physical.path);

				log_error_write(srv, __FILE__, __LINE__, "ssbsb",
						"file not found ... or so: ", strerror(errno),
						con->uri.path,
						"->", con->physical.path);

				return HANDLER_FINISHED;
			}

			/* not found, perhaps PATHINFO */

			buffer_copy_string_buffer(srv->tmp_buf, con->physical.path);

			do {
				struct stat st;

				if (slash) {
					buffer_copy_string_len(con->physical.path, srv->tmp_buf->ptr, slash - srv->tmp_buf->ptr);
				} else {
					buffer_copy_string_buffer(con->physical.path, srv->tmp_buf);
				}

				if (0 == stat(con->physical.path->ptr, &(st)) &&
				    S_ISREG(st.st_mode)) {
					found = 1;
					break;
				}

				if (pathinfo != NULL) {
					*pathinfo = '\0';
				}
				slash = strrchr(srv->tmp_buf->ptr, '/');

				if (pathinfo != NULL) {
					/* restore '/' */
					*pathinfo = '/';
				}

				if (slash) pathinfo = slash;
			} while ((found == 0) && (slash != NULL) && (slash - srv->tmp_buf->ptr > con->physical.basedir->used - 2));

			if (found == 0) {
				/* no, it really doesn't exists */
				con->http_status = 404;

				if (con->conf.log_file_not_found) {
					log_error_write(srv, __FILE__, __LINE__, "sbsb",
							"file not found:", con->uri.path,
							"->", con->physical.path);
				}

				buffer_reset(con->physical.path);

				return HANDLER_FINISHED;
			}

			/* we have a PATHINFO */
			if (pathinfo) {
				buffer_copy_string(con->request.pathinfo, pathinfo);

				/*
				 * shorten uri.path
				 */

				con->uri.path->used -= strlen(pathinfo);
				con->uri.path->ptr[con->uri.path->used - 1] = '\0';
			}

			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__,  "s",  "-- after pathinfo check");
				log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
				log_error_write(srv, __FILE__, __LINE__,  "sb", "URI          :", con->uri.path);
				log_error_write(srv, __FILE__, __LINE__,  "sb", "Pathinfo     :", con->request.pathinfo);
			}
		}

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__,  "s",  "-- handling subrequest");
			log_error_write(srv, __FILE__, __LINE__,  "sb", "Path         :", con->physical.path);
		}

		/* call the handlers */
		switch(r = plugins_call_handle_start_backend(srv, con)) {
		case HANDLER_GO_ON:
		case HANDLER_FINISHED:
			/* if we are still here, no one wanted the file; status 403 is ok I think */

		default:
			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__,  "s",  "-- subrequest finished");
			}

			/* something strange happened */
			return r;
		}
	}

	if (con->mode == DIRECT) {
		con->http_status = 403;

		TRACE("%s", "aaaaaaah, sending 403");

		return HANDLER_FINISHED;
	} else {
		return HANDLER_GO_ON;
	}
}



