#include <sys/stat.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#include "buffer.h"
#include "server.h"
#include "log.h"
#include "connections.h"
#include "fdevent.h"

#include "request.h"
#include "response.h"
#include "network.h"
#include "http_chunk.h"
#include "file_cache.h"
#include "joblist.h"
#include "file_cache_funcs.h"
#include "file_descr_funcs.h"

#include "plugin.h"

#ifdef USE_OPENSSL
# include <openssl/ssl.h> 
# include <openssl/err.h> 
#endif

#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif

#include "sys-socket.h"

static connection *connections_get_new_connection(server *srv) {
	connections *conns = srv->conns;
	size_t i;
	
	if (conns->size == 0) {
		conns->size = 128;
		conns->ptr = NULL;
		conns->ptr = malloc(sizeof(*conns->ptr) * conns->size);
		for (i = 0; i < conns->size; i++) {
			conns->ptr[i] = connection_init(srv);
		}
	} else if (conns->size == conns->used) {
		conns->size += 128;
		conns->ptr = realloc(conns->ptr, sizeof(*conns->ptr) * conns->size);
		
		for (i = conns->used; i < conns->size; i++) {
			conns->ptr[i] = connection_init(srv);
		}
	}

	connection_reset(srv, conns->ptr[conns->used]);
#if 0	
	fprintf(stderr, "%s.%d: add: ", __FILE__, __LINE__);
	for (i = 0; i < conns->used + 1; i++) {
		fprintf(stderr, "%d ", conns->ptr[i]->fd);
	}
	fprintf(stderr, "\n");
#endif	
	
	conns->ptr[conns->used]->ndx = conns->used;
	return conns->ptr[conns->used++];
}

static int connection_del(server *srv, connection *con) {
	size_t i;
	connections *conns = srv->conns;
	connection *temp;
	
	if (con == NULL) return -1;
	
	if (-1 == con->ndx) return -1;
	
	i = con->ndx;
	
	/* not last element */
	
	if (i != conns->used - 1) {
		temp = conns->ptr[i];
		conns->ptr[i] = conns->ptr[conns->used - 1];
		conns->ptr[conns->used - 1] = temp;
		
		conns->ptr[i]->ndx = i;
		conns->ptr[conns->used - 1]->ndx = -1;
	}
	
	conns->used--;
	
	con->ndx = -1;
#if 0
	fprintf(stderr, "%s.%d: del: (%d)", __FILE__, __LINE__, conns->used);
	for (i = 0; i < conns->used; i++) {
		fprintf(stderr, "%d ", conns->ptr[i]->fd);
	}
	fprintf(stderr, "\n");
#endif	
	return 0;
}

int connection_close(server *srv, connection *con) {
#ifdef USE_OPENSSL
	server_socket *srv_sock = con->srv_socket;
#endif
	
#ifdef USE_OPENSSL
	if (srv_sock->is_ssl) {
		if (con->fd->ssl) SSL_free(con->fd->ssl);
		con->fd->ssl = NULL;
	}
#endif
	
	if (con->fd->fd != -1) {
		fdevent_event_del(srv->ev, con->fd);
		fdevent_unregister(srv->ev, con->fd);
		
		file_descr_reset(con->fd);
		
		srv->cur_fds--;
	}
	
	connection_del(srv, con);
	connection_set_state(srv, con, CON_STATE_CONNECT);
	
	return 0;
}

#if 0
static void dump_packet(const unsigned char *data, size_t len) {
	size_t i, j;
	
	if (len == 0) return;
	
	for (i = 0; i < len; i++) {
		if (i % 16 == 0) fprintf(stderr, "  ");
		
		fprintf(stderr, "%02x ", data[i]);
		
		if ((i + 1) % 16 == 0) {
			fprintf(stderr, "  ");
			for (j = 0; j <= i % 16; j++) {
				unsigned char c;
				
				if (i-15+j >= len) break;
				
				c = data[i-15+j];
				
				fprintf(stderr, "%c", c > 32 && c < 128 ? c : '.');
			}
			
			fprintf(stderr, "\n");
		}
	}
	
	if (len % 16 != 0) {
		for (j = i % 16; j < 16; j++) {
			fprintf(stderr, "   ");
		}
		
		fprintf(stderr, "  ");
		for (j = i & ~0xf; j < len; j++) {
			unsigned char c;
			
			c = data[j];
			fprintf(stderr, "%c", c > 32 && c < 128 ? c : '.');
		}
		fprintf(stderr, "\n");
	}
}
#endif 

static network_t connection_handle_read(server *srv, connection *con) {
	return network_read_chunkqueue(srv, con->fd, con->read_queue);
}

static int connection_handle_write_prepare(server *srv, connection *con) {
	struct stat st;
	int s_len;
	
	switch(con->mode) {
	case DIRECT:
		/* static files */
		
		if (con->http_status == 200 && 
		    con->physical.path->used) {
			switch(con->request.http_method_id) {
			case HTTP_METHOD_GET:
			case HTTP_METHOD_POST:
			case HTTP_METHOD_HEAD:
				break;
			default:
				con->http_status = 501;
				break;
			}
		}
		
		
		switch(con->http_status) {
		case 400: /* class: header + custom body */
		case 401:
		case 403:
		case 404:
		case 408:
		case 411:
		case 416:
		case 500:
		case 501:
		case 503:
		case 505: {
			con->file_finished = 1;
				
			/* rewrite the filename */
			
			/* FIXME: use con.physical.errorfile
			 * 
			 * 
			 */
			buffer_reset(con->physical.path);
			
			if (srv->srvconf.errorfile_prefix->used) {
				buffer_copy_string_buffer(con->physical.path, srv->srvconf.errorfile_prefix);
				buffer_append_string(con->physical.path, get_http_status_body_name(con->http_status));
			}
			
			if ((con->physical.path->used <= 1) || 
			    (-1 == (stat(con->physical.path->ptr, &st)))) {
				buffer *b;
				
				buffer_reset(con->physical.path);
				
				b = chunkqueue_get_append_buffer(con->write_queue);
				
				/* build default error-page */
				buffer_copy_string(b, 
					      "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
					      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
					      "         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
					      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
					      " <head>\n"
					      "  <title>");
				buffer_append_long(b, con->http_status);
				buffer_append_string(b, " - ");
				buffer_append_string(b, get_http_status_name(con->http_status));
				
				buffer_append_string(b,
					      "</title>\n"
					      " </head>\n"
					      " <body>\n"
					      "  <h1>");
				buffer_append_long(b, con->http_status);
				buffer_append_string(b, " - ");
				buffer_append_string(b, get_http_status_name(con->http_status));
				
				buffer_append_string(b,"</h1>\n" 
					      " </body>\n"
					      "</html>\n"
					      );
				
				response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));
			} else {
				/* get content-type */
				size_t k;
				s_len = con->physical.path->used - 1;
				
				for (k = 0; k < con->conf.mimetypes->used; k++) {
					data_string *ds = (data_string *)con->conf.mimetypes->data[k];
					int ct_len = ds->key->used - 1;
					
					if (s_len < ct_len ||
					    ds->key->used == 0) continue;
					
					if (0 == strncmp(con->physical.path->ptr + s_len - ct_len, ds->key->ptr, ct_len)) {
						response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(ds->value));
						break;
					}
				}
				
				if (k == con->conf.mimetypes->used) {
					/* the error message should be HTML */
					response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));
				}
			}
		}
			/* fall through */
			
		case 200: /* class: header + body */
			if (con->physical.path->used) {
				file_cache_entry *fce = NULL;
				con->file_finished = 1;
				
				if (NULL == (fce = file_cache_get_entry(srv, con->physical.path))) {
					log_error_write(srv, __FILE__, __LINE__, "sb",
							strerror(errno), con->physical.path);
					
					connection_set_state(srv, con, CON_STATE_ERROR);
					
					return -1;
				}
				
				if (S_ISREG(fce->st.st_mode)) {
					if (con->request.http_method_id == HTTP_METHOD_GET ||
					    con->request.http_method_id == HTTP_METHOD_POST) {
						http_chunk_append_file(srv, con, fce, 0, fce->st.st_size);
						con->response.content_length = http_chunkqueue_length(srv, con);
					} else if (con->request.http_method_id == HTTP_METHOD_HEAD) {
						con->response.content_length = fce->st.st_size;
					} else {
						connection_set_state(srv, con, CON_STATE_ERROR);
						return -1;
					}
					
					http_response_write_header(srv, con, 
								   con->response.content_length, 
								   fce->st.st_mtime);
					
					
				} else {
					/* why the heck ? */
					
					log_error_write(srv, __FILE__, __LINE__, "sb",
							"connection closed: no regular-file to send:", 
							con->physical.path);
					
					con->file_finished = 1;
				}
			} else {
				if (con->file_finished) {
					con->response.content_length = (ssize_t)http_chunkqueue_length(srv, con);
				}
				
				/* disable keep-alive if size-info for the body is missing */
				if ((con->parsed_response & HTTP_CONTENT_LENGTH) &&
				    ((con->response.transfer_encoding & HTTP_TRANSFER_ENCODING_CHUNKED) == 0)) {
					con->keep_alive = 0;
				}
				
				if (con->request.http_method_id == HTTP_METHOD_HEAD) {
					chunkqueue_reset(con->write_queue);
				}
				
				http_response_write_header(srv, con, 
							   con->response.content_length, 
							   0);
				
			}
			break;
			
		case 206: /* write_queue is already prepared */
			http_response_write_header(srv, con, 
						   con->response.content_length,
						   0);
			con->file_finished = 1;
			break;
		case 302:
			con->file_finished = 1;
			
			con->response.content_length = (ssize_t)http_chunkqueue_length(srv, con);
				
			http_response_write_header(srv, con, 
						   con->response.content_length, 
						   0);
			
			break;
		case 205: /* class: header only */
		case 301:
		case 304:
		default:
			/* disable chunked encoding again as we have no body */
			con->response.transfer_encoding &= ~HTTP_TRANSFER_ENCODING_CHUNKED;
			chunkqueue_reset(con->write_queue);
			
			http_response_write_header(srv, con, 0, 0);
			con->file_finished = 1;
			break;
		}
		
		break;
	default:
		if (con->request.http_method_id == HTTP_METHOD_HEAD ||
		    con->http_status == 301 ||
		    con->http_status == 304 ||
		    con->http_status == 205) {
			/* remove possible chunks */
			con->response.transfer_encoding &= ~HTTP_TRANSFER_ENCODING_CHUNKED;
			chunkqueue_reset(con->write_queue);
		}
		
		if (0 == (con->parsed_response & HTTP_CONNECTION)) {
			/* (f)cgi did'nt send Connection: header 
			 * 
			 * shall we ?
			 */
			if (((con->response.transfer_encoding & HTTP_TRANSFER_ENCODING_CHUNKED) == 0) &&
			    (con->parsed_response & HTTP_CONTENT_LENGTH) == 0) {
				/* without content_length, no keep-alive */
				
				con->keep_alive = 0;
			}
		} else {
			/* a subrequest disable keep-alive although the client wanted it */
			if (con->keep_alive && !con->response.keep_alive) {
				con->keep_alive = 0;
				
				/* FIXME: we have to drop the Connection: Header from the subrequest */
			}
		}
		
		con->response.content_length = (ssize_t)http_chunkqueue_length(srv, con);
		http_response_write_basic_header(srv, con);
			
		break;
	}
	
	return 0;
}

static network_t network_write_traffic_shaped(server *srv, connection *con, chunkqueue *cq) {
	int ret = -1;
	off_t written = 0;
	
	if (con->conf.global_kbytes_per_second &&
	    *(con->conf.global_bytes_per_second_cnt_ptr) > con->conf.global_kbytes_per_second * 1024) {
		/* we reached the global traffic limit */

		con->traffic_limit_reached = 1;
		joblist_append(srv, con);

		return NETWORK_OK;
	}  

	written = con->fd->bytes_written;

	ret = network_write_chunkqueue(srv, con->fd, cq);

	written = con->fd->bytes_written - written;
	con->bytes_written_cur_second += written;

	*(con->conf.global_bytes_per_second_cnt_ptr) += written;
	
	if (con->conf.kbytes_per_second &&
	    (con->bytes_written_cur_second > con->conf.kbytes_per_second * 1024)) {
		/* we reached the traffic limit */

		con->traffic_limit_reached = 1;
		joblist_append(srv, con);
	}  
	return ret;
}


static int connection_handle_write(server *srv, connection *con) {
	network_t ret;
	switch(ret = network_write_traffic_shaped(srv, con, con->write_queue)) {
	case NETWORK_QUEUE_EMPTY:
		if (con->file_finished) {
			connection_set_state(srv, con, CON_STATE_RESPONSE_END);
			joblist_append(srv, con);
		}
		break;
	case NETWORK_OK:
		/* not finished yet, queue not empty yet, wait for event */
		break;
	case NETWORK_ERROR: /* error on our side */
		log_error_write(srv, __FILE__, __LINE__, "sd",
				"connection closed: write failed on fd", con->fd->fd);
		connection_set_state(srv, con, CON_STATE_ERROR);
		joblist_append(srv, con);
		break;
	case NETWORK_REMOTE_CLOSE: /* remote close */
		connection_set_state(srv, con, CON_STATE_ERROR);
		joblist_append(srv, con);
		break;
	default:
		log_error_write(srv, __FILE__, __LINE__, "sd",
				"unknown code:", ret);
		break;
	}
	
	return 0;
}



connection *connection_init(server *srv) {
	connection *con;
	
	UNUSED(srv);

	con = calloc(1, sizeof(*con));
		
	con->fd = file_descr_init();

	con->ndx = -1;
	con->bytes_written = 0;
	con->bytes_read = 0;
	con->bytes_header = 0;
	

#define CLEAN(x) \
	con->x = buffer_init();
	
	CLEAN(request.uri);
	CLEAN(request.request_line);
	CLEAN(request.request);
	CLEAN(request.pathinfo);
	CLEAN(request.content);
	CLEAN(request.http_method_name);
	
	CLEAN(request.orig_uri);
	
	CLEAN(uri.scheme);
	CLEAN(uri.authority);
	CLEAN(uri.path);
	CLEAN(uri.path_raw);
	CLEAN(uri.query);
	
	CLEAN(physical.doc_root);
	CLEAN(physical.path);
	CLEAN(physical.rel_path);
	CLEAN(physical.etag);
	CLEAN(parse_request);
	
	CLEAN(authed_user);
	CLEAN(server_name);
	CLEAN(error_handler);
	
#undef CLEAN
	con->write_queue = chunkqueue_init();
	con->read_queue = chunkqueue_init();
	con->request.headers      = array_init();
	con->response.headers     = array_init();
	con->environment     = array_init();
	
	/* init plugin specific connection structures */
	
	con->plugin_ctx = calloc(srv->plugins.used + 1, sizeof(void *));
	
	config_setup_connection(srv, con);
	
	return con;
}

void connections_free(server *srv) {
	connections *conns = srv->conns;
	size_t i;	
	
	for (i = 0; i < conns->size; i++) {
		connection *con = conns->ptr[i];
		
		connection_reset(srv, con);
		
		chunkqueue_free(con->write_queue);
		chunkqueue_free(con->read_queue);
		array_free(con->request.headers);
		array_free(con->response.headers);
		array_free(con->environment);

#define CLEAN(x) \
	buffer_free(con->x);
		
		CLEAN(request.uri);
		CLEAN(request.request_line);
		CLEAN(request.request);
		CLEAN(request.pathinfo);
		CLEAN(request.content);
		
		CLEAN(request.orig_uri);
		
		CLEAN(uri.scheme);
		CLEAN(uri.authority);
		CLEAN(uri.path);
		CLEAN(uri.path_raw);
		CLEAN(uri.query);
		
		CLEAN(physical.doc_root);
		CLEAN(physical.path);
		CLEAN(physical.etag);
		CLEAN(physical.rel_path);
		CLEAN(parse_request);
		
		CLEAN(authed_user);
		CLEAN(server_name);
		CLEAN(error_handler);
#undef CLEAN
		free(con->plugin_ctx);

		file_descr_free(con->fd);
		
		free(con);
	}
	
	free(conns->ptr);
}


int connection_reset(server *srv, connection *con) {
	size_t i;
	
	plugins_call_connection_reset(srv, con);

	con->http_status = 0;
	con->file_finished = 0;
	con->file_started = 0;
	con->got_response = 0;
	
	con->parsed_response = 0;
	
	con->bytes_written = 0;
	con->bytes_written_cur_second = 0;
	con->bytes_read = 0;
	con->bytes_header = 0;
	
	con->request.http_method_id = HTTP_METHOD_UNSET;
	con->request.http_version = HTTP_VERSION_UNSET;
	
	con->request.http_if_modified_since = NULL;
	con->request.http_if_none_match = NULL;
	
	con->response.keep_alive = 0;
	con->response.content_length = -1;
	con->response.transfer_encoding = 0;
	
	con->mode = DIRECT;
	
#define CLEAN(x) \
	if (con->x) buffer_reset(con->x);
	
	CLEAN(request.uri);
	CLEAN(request.request_line);
	CLEAN(request.pathinfo);
	CLEAN(request.content);
	CLEAN(request.request);
	CLEAN(request.http_method_name);
	
	CLEAN(request.orig_uri);
	
	CLEAN(uri.scheme);
	CLEAN(uri.authority);
	CLEAN(uri.path);
	CLEAN(uri.path_raw);
	CLEAN(uri.query);
	
	CLEAN(physical.doc_root);
	CLEAN(physical.path);
	CLEAN(physical.rel_path);
	CLEAN(physical.etag);
	
	CLEAN(parse_request);
	
	CLEAN(authed_user);
	CLEAN(server_name);
	CLEAN(error_handler);
#undef CLEAN	
	
#define CLEAN(x) \
	if (con->x) con->x->used = 0;	
	
#undef CLEAN
	
#define CLEAN(x) \
		con->request.x = NULL;
	
	CLEAN(http_host);
	CLEAN(http_range);
	CLEAN(http_content_type);
#undef CLEAN
	con->request.content_length = 0;
	
	array_reset(con->request.headers);
	array_reset(con->response.headers);
	array_reset(con->environment);
	
	chunkqueue_reset(con->write_queue);
	
	for (i = 0; i < srv->plugins.used; i++) {
		con->plugin_ctx[0] = NULL;
	}
	
	con->header_len = 0;
	con->in_error_handler = 0;
	
	config_setup_connection(srv, con);
	
	return 0;
}

/**
 * 
 * search for \r\n\r\n 
 * 
 * this is a special 32bit version which is using a sliding window for
 * the comparisions 
 * 
 * how it works:
 * 
 * b:      'abcdefg'
 * rnrn:   'cdef'
 * 
 * cmpbuf: abcd != cdef
 * cmpbuf: bcde != cdef
 * cmpbuf: cdef == cdef -> return &c
 * 
 * cmpbuf and rnrn are treated as 32bit uint and bit-ops are used to 
 * maintain cmpbuf and rnrn
 * 
 */

char *buffer_search_rnrn(buffer *b) {
	uint32_t cmpbuf, rnrn;
	char *cp;
	size_t i;
	
	if (b->used < 4) return NULL;
	
	rnrn = ('\r' << 24) | ('\n' << 16) |
		('\r' << 8) | ('\n' << 0);
	
	cmpbuf = (b->ptr[0] << 24) | (b->ptr[1] << 16) |
		(b->ptr[2] << 8) | (b->ptr[3] << 0);
		
	cp = b->ptr + 4;
	for (i = 0; i < b->used - 4; i++) {
		if (cmpbuf == rnrn) return cp - 4;
			
		cmpbuf = (cmpbuf << 8 | *(cp++)) & 0xffffffff;
	}
	
	return NULL;
}

int connection_handle_read_state(server *srv, connection *con)  {
	int ostate = con->state;
	char *h_term = NULL;
	chunk *c;
	chunkqueue *cq = con->read_queue;
	handler_t r;
	
	if (con->fd->is_readable) {
		network_t nr;
		con->read_idle_ts = srv->cur_ts;
		
		switch(nr = connection_handle_read(srv, con)) {
		case NETWORK_OK:
			break;
		case NETWORK_ERROR:
			return -1;
		case NETWORK_REMOTE_CLOSE:
			connection_set_state(srv, con, CON_STATE_ERROR);
			return 0;
		default:
			break;
		}
	}

	/* move the empty chunks out of the way */
	chunkqueue_remove_empty_chunks(cq);
	
	/* nothing to handle */
	if (chunkqueue_is_empty(cq)) return 0;
	
	switch(ostate) {
	case CON_STATE_READ:
		/* prepare con->request.request */
		c = cq->first;
		
		/* check if we need the full package */
		if (con->request.request->used == 0) {
			buffer b;
			
			b.ptr = c->data.mem->ptr + c->offset;
			b.used = c->data.mem->used - c->offset;
			
			if (NULL != (h_term = buffer_search_rnrn(&b))) {
				/* \r\n\r\n found
				 * - copy everything incl. the terminator to request.request
				 */
				
				buffer_copy_string_len(con->request.request, 
						       b.ptr, 
						       h_term - b.ptr + 4);
				
				/* the buffer has been read up to the terminator */
				c->offset += h_term - b.ptr + 4;
			} else {
				/* not found, copy everything */
				buffer_copy_string_len(con->request.request, c->data.mem->ptr + c->offset, c->data.mem->used - c->offset - 1);
				c->offset = c->data.mem->used - 1;
			}
		} else {
			/* have to take care of overlapping header terminators */
			
			size_t l = con->request.request->used - 2;
			char *s  = con->request.request->ptr;
			buffer b;
			
			b.ptr = c->data.mem->ptr + c->offset;
			b.used = c->data.mem->used - c->offset;
			
			if (con->request.request->used - 1 > 3 &&
			    c->data.mem->used > 1 &&
			    s[l-2] == '\r' &&
			    s[l-1] == '\n' &&
			    s[l-0] == '\r' &&
			    c->data.mem->ptr[0] == '\n') {
				buffer_append_string_len(con->request.request, c->data.mem->ptr + c->offset, 1);
				c->offset += 1;
				
				h_term = con->request.request->ptr;
			} else if (con->request.request->used - 1 > 2 &&
				   c->data.mem->used > 2 &&
				   s[l-1] == '\r' &&
				   s[l-0] == '\n' &&
				   c->data.mem->ptr[0] == '\r' &&
				   c->data.mem->ptr[1] == '\n') {
				buffer_append_string_len(con->request.request, c->data.mem->ptr + c->offset, 2);
				c->offset += 2;
				
				h_term = con->request.request->ptr;
			} else if (con->request.request->used - 1 > 1 &&
				   c->data.mem->used > 3 &&
				   s[l-0] == '\r' &&
				   c->data.mem->ptr[0] == '\n' &&
				   c->data.mem->ptr[1] == '\r' &&
				   c->data.mem->ptr[2] == '\n') {
				buffer_append_string_len(con->request.request, c->data.mem->ptr + c->offset, 3);
				c->offset += 3;
				
				h_term = con->request.request->ptr;
			} else if (NULL != (h_term = buffer_search_string_len(&b, "\r\n\r\n", 4))) {
				/* \r\n\r\n found
				 * - copy everything incl. the terminator to request.request
				 */
				
				buffer_append_string_len(con->request.request, 
						       c->data.mem->ptr + c->offset, 
						       c->offset + h_term - b.ptr + 4);
				
				/* the buffer has been read up to the terminator */
				c->offset += h_term - b.ptr + 4;
			} else {
				/* not found, copy everything */
				buffer_append_string_len(con->request.request, c->data.mem->ptr + c->offset, c->data.mem->used - c->offset - 1);
				c->offset = c->data.mem->used - 1;
			}
		}
		
		chunkqueue_remove_empty_chunks(cq);
		
		/* con->request.request is setup up */
		if (h_term) {
			connection_set_state(srv, con, CON_STATE_REQUEST_END);
		} else if (chunkqueue_length(cq) > 64 * 1024) {
			log_error_write(srv, __FILE__, __LINE__, "sd", "http-header larger then 64k -> disconnected", chunkqueue_length(cq));
			connection_set_state(srv, con, CON_STATE_ERROR);
		}
		break;
	case CON_STATE_HANDLE_REQUEST: 
		if (con->request.content_finished) break;		
		
		switch(r = plugins_call_handle_fetch_post_data(srv, con)) {
		case HANDLER_FINISHED:
			/* another plugin collected the data */
			
			break;
		case HANDLER_GO_ON:
			/* no-one else fetched it, so we fall back to fetch it into request.content */
			for (c = cq->first; c && (con->request.content->used != con->request.content_length + 1); c = cq->first) {
				off_t weWant, weHave, toRead;
			
				weWant = con->request.content_length - (con->request.content->used ? con->request.content->used - 1 : 0);
				/* without the terminating \0 */
			
				assert(c->data.mem->used);
			
				weHave = c->data.mem->used - c->offset - 1;
				
				toRead = weHave > weWant ? weWant : weHave;
			
				buffer_append_string_len(con->request.content, c->data.mem->ptr + c->offset, toRead);
			
				c->offset += toRead;
				
				chunkqueue_remove_empty_chunks(cq);
			}
		
			/* Content is ready */
			if (con->request.content->used == con->request.content_length + 1) {
				con->request.content_finished = 1;
			}

			break;
		case HANDLER_COMEBACK:
		case HANDLER_WAIT_FOR_EVENT:
		case HANDLER_ERROR:
			return r;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sd", "handle_uri_raw: unknown return value", r);
			break;
		}
			
		break;
	}
	
	return 0;
}

handler_t connection_handle_fdevent(void *s, void *context, int revents) {
	server     *srv = (server *)s;
	connection *con = context;
	
	joblist_append(srv, con);
	
	if (revents & FDEVENT_IN) {
		con->fd->is_readable = 1;
#if 0
		log_error_write(srv, __FILE__, __LINE__, "sd", "read-wait - done", con->fd->fd);
#endif
	}
	if (revents & FDEVENT_OUT) {
		con->fd->is_writable = 1;
		/* we don't need the event twice */
	}
	
	
	if (revents & ~(FDEVENT_IN | FDEVENT_OUT)) {
		/* looks like an error */
						
		/* FIXME: revents = 0x19 still means that we should read from the queue */
		if (revents & FDEVENT_HUP) {
			if (con->state == CON_STATE_CLOSE) {
				con->close_timeout_ts = 0;
			} else {
				/* sigio reports the wrong event here
				 * 
				 * there was no HUP at all 
				 */
#ifdef USE_LINUX_SIGIO
				if (srv->ev->in_sigio == 1) {
					log_error_write(srv, __FILE__, __LINE__, "sd",
						"connection closed: poll() -> HUP", con->fd->fd);
				} else {
					connection_set_state(srv, con, CON_STATE_ERROR);
				}
#else
				connection_set_state(srv, con, CON_STATE_ERROR);
#endif
				
			}
		} else if (revents & FDEVENT_ERR) {
#ifndef USE_LINUX_SIGIO
			log_error_write(srv, __FILE__, __LINE__, "sd",
					"connection closed: poll() -> ERR", con->fd->fd);
#endif	
			connection_set_state(srv, con, CON_STATE_ERROR);
		} else {
			log_error_write(srv, __FILE__, __LINE__, "sd",
					"connection closed: poll() -> ???", revents);
		} 
	}
	
	if (con->state == CON_STATE_READ ||
	    (con->state == CON_STATE_HANDLE_REQUEST && con->request.content_finished == 0)) {
		if (-1 == connection_handle_read_state(srv, con)) {
			connection_set_state(srv, con, CON_STATE_ERROR);
			
			log_error_write(srv, __FILE__, __LINE__, "ds",
					con->fd->fd,
					"handle write failed.");
		}
	}
	
	if (con->state == CON_STATE_WRITE &&
	    !chunkqueue_is_empty(con->write_queue) &&
	    con->fd->is_writable) {
		
		if (-1 == connection_handle_write(srv, con)) {
			connection_set_state(srv, con, CON_STATE_ERROR);
			
			log_error_write(srv, __FILE__, __LINE__, "ds",
					con->fd->fd,
					"handle write failed.");
		} else if (con->state == CON_STATE_WRITE) {
			con->write_request_ts = srv->cur_ts;
		}
	}
	
	if (con->state == CON_STATE_CLOSE) {
		/* flush the read buffers */
		int b;
		
		if (ioctl(con->fd->fd, FIONREAD, &b)) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
					"ioctl() failed", strerror(errno));
		}
		
		if (b > 0) {
			char buf[1024];
			log_error_write(srv, __FILE__, __LINE__, "sdd",
					"CLOSE-read()", con->fd->fd, b);
			
			/* */
			read(con->fd->fd, buf, sizeof(buf));
		} else {
			/* nothing to read */
			
			con->close_timeout_ts = 0;
		}
	}
	
	return HANDLER_FINISHED;
}


connection *connection_accept(server *srv, server_socket *srv_socket) {
	int accepted_requests = 0;
	/* accept everything */

	/* search an empty place */
	int cnt;
	sock_addr cnt_addr;
	socklen_t cnt_len;
	/* accept it and register the fd */
	
	cnt_len = sizeof(cnt_addr);

	if (-1 == (cnt = accept(srv_socket->fd->fd, (struct sockaddr *) &cnt_addr, &cnt_len))) {
		if ((errno != EAGAIN) &&
		    (errno != EINTR)) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "accept failed: ", strerror(errno));
		}
		return NULL;
	} else {
		connection *con;
		
		srv->cur_fds++;
		
		accepted_requests++;
		/* ok, we have the connection, register it */
#if 0
		log_error_write(srv, __FILE__, __LINE__, "sd",
				"appected()", cnt);
#endif
		srv->con_opened++;
		
		con = connections_get_new_connection(srv);
		
		con->fd->fd = cnt;
#if 0		
		gettimeofday(&(con->start_tv), NULL);
#endif		
		fdevent_register(srv->ev, con->fd, connection_handle_fdevent, con);
		
		connection_set_state(srv, con, CON_STATE_REQUEST_START);
		
		con->connection_start = srv->cur_ts;
		con->dst_addr = cnt_addr;
		con->srv_socket = srv_socket;
		
		if (-1 == (fdevent_fcntl_set(srv->ev, con->fd))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "fcntl failed: ", strerror(errno));
			return NULL;
		}
#ifdef USE_OPENSSL
		/* connect FD to SSL */
		if (srv_socket->is_ssl) {
			if (NULL == (con->fd->ssl = SSL_new(srv_socket->ssl_ctx))) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
						ERR_error_string(ERR_get_error(), NULL));
				
				return NULL;
			} else {
				SSL_set_accept_state(con->fd->ssl);
			}
			
			if (1 != (SSL_set_fd(con->fd->ssl, cnt))) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
						ERR_error_string(ERR_get_error(), NULL));
				return NULL;
			}
		}
#endif
		return con;
	}
}


int connection_state_machine(server *srv, connection *con) {
	int done = 0, r;
#ifdef USE_OPENSSL
	server_socket *srv_sock = con->srv_socket;
#endif
	
	if (srv->srvconf.log_state_handling) {
		log_error_write(srv, __FILE__, __LINE__, "sds", 
				"state at start", 
				con->fd->fd,
				connection_get_state(con->state));
	}

	while (done == 0) {
		size_t ostate = con->state;
		int b;
		
		switch (con->state) {
		case CON_STATE_REQUEST_START: /* transient */
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			con->request_start = srv->cur_ts;
			con->read_idle_ts = srv->cur_ts;
			
			con->request_count++;
			
			connection_set_state(srv, con, CON_STATE_READ);
			
			break;
		case CON_STATE_REQUEST_END: /* transient */
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			http_request_parse(srv, con);
			
			connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);
			
			break;
		case CON_STATE_HANDLE_REQUEST:
			/* 
			 * the request is parsed
			 * 
			 * decided what to do with the request
			 * - 
			 * 
			 * 
			 */
			
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			switch (r = http_response_prepare(srv, con)) {
			case HANDLER_FINISHED:
				if (con->http_status == 404 ||
				    con->http_status == 403) {
					/* 404 error-handler */
					
					if (con->in_error_handler == 0 && 
					    (!buffer_is_empty(con->conf.error_handler) ||
					     !buffer_is_empty(con->error_handler))) {
						/* call error-handler */
						
						con->error_handler_saved_status = con->http_status;
						con->http_status = 0;
						
						if (buffer_is_empty(con->error_handler)) {
							buffer_copy_string_buffer(con->request.uri, con->conf.error_handler);
						} else {
							buffer_copy_string_buffer(con->request.uri, con->error_handler);
						}
						buffer_reset(con->physical.path);
						
						con->in_error_handler = 1;
						
						connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);
						
						done = -1;
						break;
					} else if (con->in_error_handler) {
						/* error-handler is a 404 */
						
						/* continue as normal, status is the same */
						log_error_write(srv, __FILE__, __LINE__, "sb", "error-handler not found:", con->conf.error_handler);
						
						con->http_status = con->error_handler_saved_status;
					}
				} else if (con->in_error_handler) {
					/* error-handler is back and has generated content */
					/* reset the old status */
					if (con->http_status == 0) {
						con->http_status = con->error_handler_saved_status;
					}
				}
				
				if (con->http_status == 0) con->http_status = 200;
				
				/* we have something to send, go on */
				connection_set_state(srv, con, CON_STATE_RESPONSE_START);
				break;
			case HANDLER_WAIT_FOR_FD:
				srv->want_fds++;
				
				fdwaitqueue_append(srv, con);
				
				connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);
				
				break;
			case HANDLER_COMEBACK:
				done = -1;
			case HANDLER_WAIT_FOR_EVENT:
				/* come back here */
				connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);
				
				break;
			case HANDLER_ERROR:
				/* something went wrong */
				connection_set_state(srv, con, CON_STATE_ERROR);
				break;
			default:
				log_error_write(srv, __FILE__, __LINE__, "sdd", "unknown ret-value: ", con->fd->fd, r);
				break;
			}
			
			if (con->request.content_finished == 0) {
				if (-1 == connection_handle_read_state(srv, con)) {
					connection_set_state(srv, con, CON_STATE_ERROR);
			
					log_error_write(srv, __FILE__, __LINE__, "ds",
							con->fd->fd,
							"handle write failed.");
				}
			}

			break;
		case CON_STATE_RESPONSE_START:
			/* 
			 * the decision is done
			 * - create the HTTP-Response-Header
			 * 
			 */
			
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			connection_handle_write_prepare(srv, con);
			
			connection_set_state(srv, con, CON_STATE_WRITE);
			break;
		case CON_STATE_RESPONSE_END: /* transient */
			/* log the request */
			
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			plugins_call_handle_request_done(srv, con);
			
			srv->con_written++;
			
			if (con->keep_alive) {
				connection_set_state(srv, con, CON_STATE_REQUEST_START);
				
#if 0					
				con->request_start = srv->cur_ts;
				con->read_idle_ts = srv->cur_ts;
#endif
			} else {
				switch(r = plugins_call_handle_connection_close(srv, con)) {
				case HANDLER_GO_ON:
				case HANDLER_FINISHED:
					break;
				default:
					log_error_write(srv, __FILE__, __LINE__, "sd", "unhandling return value", r);
					break;
				}
				
#ifdef USE_OPENSSL
				if (srv_sock->is_ssl) {
					switch (SSL_shutdown(con->fd->ssl)) {
					case 1:
						/* done */
						break;
					case 0:
						/* wait for fd-event 
						 * 
						 * FIXME: wait for fdevent and call SSL_shutdown again
						 * 
						 */
						
						break;
					default:
						log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
								ERR_error_string(ERR_get_error(), NULL));
					}
				}
#endif
				connection_close(srv, con);
				
				srv->con_closed++;
			}
			
			connection_reset(srv, con);
			
			break;
		case CON_STATE_CONNECT:
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			chunkqueue_reset(con->read_queue);
			
			con->request_count = 0;
			
			break;
		case CON_STATE_CLOSE:
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			if (con->keep_alive) {
				if (ioctl(con->fd->fd, FIONREAD, &b)) {
					log_error_write(srv, __FILE__, __LINE__, "ss",
							"ioctl() failed", strerror(errno));
				}
				if (b > 0) {
					char buf[1024];
					log_error_write(srv, __FILE__, __LINE__, "sdd",
							"CLOSE-read()", con->fd->fd, b);
					
					/* */
					read(con->fd->fd, buf, sizeof(buf));
				} else {
					/* nothing to read */
					
					con->close_timeout_ts = 0;
				}
			} else {
				con->close_timeout_ts = 0;
			}
			
			if (srv->cur_ts - con->close_timeout_ts > 1) {
				connection_close(srv, con);
				
				if (srv->srvconf.log_state_handling) {
					log_error_write(srv, __FILE__, __LINE__, "sd", 
							"connection closed for fd", con->fd->fd);
				}
			}
			
			break;
		case CON_STATE_READ:
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			/* only the first round will call _read_state()
			 * the next ones will be handled by fdevent() */
			if (-1 == connection_handle_read_state(srv, con)) {
				log_error_write(srv, __FILE__, __LINE__, "sd", 
						"read failed for fd", con->fd->fd);
				connection_set_state(srv, con, CON_STATE_ERROR);
			}
			break;
		case CON_STATE_WRITE:
			if (srv->srvconf.log_state_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sds", 
						"state for fd", con->fd->fd, connection_get_state(con->state));
			}
			
			/* only try to write if we have something in the queue */
			if (!chunkqueue_is_empty(con->write_queue)) {
#if 0
				log_error_write(srv, __FILE__, __LINE__, "dsd",
						con->fd->fd,
						"packets to write:",
						con->write_queue->used);
#endif
			}
			if (!chunkqueue_is_empty(con->write_queue) && con->fd->is_writable) {
				if (-1 == connection_handle_write(srv, con)) {
					log_error_write(srv, __FILE__, __LINE__, "ds",
							con->fd->fd,
							"handle write failed.");
					connection_set_state(srv, con, CON_STATE_ERROR);
				} else if (con->state == CON_STATE_WRITE) {
					con->write_request_ts = srv->cur_ts;
				}
			}
			
			break;
		case CON_STATE_ERROR: /* transient */
			
			/* even if the connection was drop we still have to write it to the access log */
			if (con->http_status) {
				plugins_call_handle_request_done(srv, con);
			}
#ifdef USE_OPENSSL
			if (srv_sock->is_ssl) {
				int ret;
				switch ((ret = SSL_shutdown(con->fd->ssl))) {
				case 1:
					/* ok */
					break;
				case 0:
					SSL_shutdown(con->fd->ssl);
					break;
				default:
					log_error_write(srv, __FILE__, __LINE__, "sds", "SSL:", 
							SSL_get_error(con->fd->ssl, ret), 
							ERR_error_string(ERR_get_error(), NULL));
					return -1;
				}
			}
#endif
			
			switch(con->mode) {
			case DIRECT:
#if 0
				log_error_write(srv, __FILE__, __LINE__, "sd", 
						"emergency exit: direct", 
						con->fd->fd);
#endif
				break;
			default:
				switch(r = plugins_call_handle_connection_close(srv, con)) {
				case HANDLER_GO_ON:
				case HANDLER_FINISHED:
					break;
				default:
					log_error_write(srv, __FILE__, __LINE__, "");
					break;
				}
				break;
			}
			
			connection_reset(srv, con);
			
			/* close the connection */
			if ((con->keep_alive == 1) &&
			    (0 == shutdown(con->fd->fd, SHUT_WR))) {
				con->close_timeout_ts = srv->cur_ts;
				
				connection_set_state(srv, con, CON_STATE_CLOSE);
				
				if (srv->srvconf.log_state_handling) {
					log_error_write(srv, __FILE__, __LINE__, "sd", 
							"shutdown for fd", con->fd->fd);
				}
			} else {
				connection_close(srv, con);
			}
			
			con->keep_alive = 0;
			
			srv->con_closed++;
			
			break;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sdd", 
					"unknown state:", con->fd->fd, con->state);
			
			break;
		}
		
		if (done == -1) {
			done = 0;
		} else if (ostate == con->state) {
			done = 1;
		}
	}

	if (srv->srvconf.log_state_handling) {
		log_error_write(srv, __FILE__, __LINE__, "sds", 
				"state at exit:", 
				con->fd->fd,
				connection_get_state(con->state));
	}
	
	switch(con->state) {
	case CON_STATE_HANDLE_REQUEST:
		/* we have all our content, no need for an event */
		if (con->request.content_finished) {
			fdevent_event_del(srv->ev, con->fd);
			break;
		}
	case CON_STATE_READ:
	case CON_STATE_CLOSE:
		fdevent_event_add(srv->ev, con->fd, FDEVENT_IN);
		break;
	case CON_STATE_WRITE:
		/* request write-fdevent only if we really need it 
		 * - if we have data to write
		 * - if the socket is not writable yet 
		 */
		if (!chunkqueue_is_empty(con->write_queue) && 
		    (con->fd->is_writable == 0) &&
		    (con->traffic_limit_reached == 0)) {
			fdevent_event_add(srv->ev, con->fd, FDEVENT_OUT);
		} else {
			fdevent_event_del(srv->ev, con->fd);
		}
		break;
	default:
		if (con->fd->fd != -1 && con->fd->fde_ndx != -1) fdevent_event_del(srv->ev, con->fd);
		break;
	}

	return 0;
}
