#include <sys/types.h>
#ifdef __WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#endif

#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fdevent.h>
#include <signal.h>
#include <ctype.h>
#include <assert.h>

#include <stdio.h>

#include "server.h"
#include "keyvalue.h"
#include "log.h"
#include "connections.h"
#include "joblist.h"
#include "http_chunk.h"
#include "file_descr_funcs.h"
#include "network.h"
#include "chunk.h"

#include "plugin.h"

enum {EOL_UNSET, EOL_N, EOL_RN};

typedef struct {
	char **ptr;
	
	size_t size;
	size_t used;
} char_array;

typedef struct {
	pid_t *ptr;
	size_t used;
	size_t size;
} buffer_pid_t;

typedef struct {
	array *cgi;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	buffer_pid_t cgi_pid;
	
	buffer *tmp_buf;
	buffer *parse_response;
	
	plugin_config **config_storage;
	
	plugin_config conf; 
} plugin_data;

typedef struct {
	pid_t pid;
	file_descr *read_fd, *write_fd;

	ssize_t post_data_fetched;
	chunkqueue *write_queue;
	
	int read_fde_ndx, write_fde_ndx; /* index into the fd-event buffer */
	
	connection *remote_conn;  /* dumb pointer */
	plugin_data *plugin_data; /* dumb pointer */
	
	buffer *response;
	buffer *response_header;
} handler_ctx;

static handler_ctx * cgi_handler_ctx_init() {
	handler_ctx *hctx = calloc(1, sizeof(*hctx));

	assert(hctx);

	hctx->post_data_fetched = 0;

	hctx->read_fd = file_descr_init();
	hctx->write_fd = file_descr_init();
	
	hctx->read_fd->read_func = network_read_chunkqueue_write;
	hctx->write_fd->write_func = network_write_chunkqueue_write;
	
	hctx->read_fd->is_socket = 0;
	hctx->write_fd->is_socket = 0;
	
	hctx->response = buffer_init();
	hctx->response_header = buffer_init();

	hctx->write_queue = chunkqueue_init();
	
	return hctx;
}

static void cgi_handler_ctx_free(handler_ctx *hctx) {
	buffer_free(hctx->response);
	buffer_free(hctx->response_header);

	file_descr_free(hctx->write_fd);
	file_descr_free(hctx->read_fd);

	chunkqueue_free(hctx->write_queue);
	
	free(hctx);
}

enum {FDEVENT_HANDLED_UNSET, FDEVENT_HANDLED_FINISHED, FDEVENT_HANDLED_NOT_FINISHED, FDEVENT_HANDLED_ERROR};

INIT_FUNC(mod_cgi_init) {
	plugin_data *p;
	
	p = calloc(1, sizeof(*p));

	assert(p);
	
	p->tmp_buf = buffer_init();
	p->parse_response = buffer_init();
	
	return p;
}


FREE_FUNC(mod_cgi_free) {
	plugin_data *p = p_d;
	buffer_pid_t *r = &(p->cgi_pid);
	
	UNUSED(srv);
	
	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];
			
			array_free(s->cgi);
			
			free(s);
		}
		free(p->config_storage);
	}
	

	if (r->ptr) free(r->ptr);
	
	buffer_free(p->tmp_buf);
	buffer_free(p->parse_response);
	
	free(p);
	
	return HANDLER_GO_ON;
}

SETDEFAULTS_FUNC(mod_fastcgi_set_defaults) {
	plugin_data *p = p_d;
	size_t i = 0;
	
	config_values_t cv[] = { 
		{ "cgi.assign",                  NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ NULL,                          NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET}
	};

	if (!p) return HANDLER_ERROR;
	
	p->config_storage = malloc(srv->config_context->used * sizeof(specific_config *));
	
	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;
		
		s = malloc(sizeof(plugin_config));
		assert(s);
		
		s->cgi    = array_init();
		
		cv[0].destination = s->cgi;
		
		p->config_storage[i] = s;
	
		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
			return HANDLER_ERROR;
		}
	}
	
	return HANDLER_GO_ON;
}


static int cgi_pid_add(server *srv, plugin_data *p, pid_t pid) {
	int m = -1;
	size_t i;
	buffer_pid_t *r = &(p->cgi_pid);
	
	UNUSED(srv);

	for (i = 0; i < r->used; i++) {
		if (r->ptr[i] > m) m = r->ptr[i];
	}
	
	if (r->size == 0) {
		r->size = 16;
		r->ptr = malloc(sizeof(*r->ptr) * r->size);
	} else if (r->used == r->size) {
		r->size += 16;
		r->ptr = realloc(r->ptr, sizeof(*r->ptr) * r->size);
	}
	
	r->ptr[r->used++] = pid;
	
	return m;
}

static int cgi_pid_del(server *srv, plugin_data *p, pid_t pid) {
	size_t i;
	buffer_pid_t *r = &(p->cgi_pid);
	
	UNUSED(srv);

	for (i = 0; i < r->used; i++) {
		if (r->ptr[i] == pid) break;
	}
	
	if (i != r->used) {
		/* found */
		
		if (i != r->used - 1) {
			r->ptr[i] = r->ptr[r->used - 1];
		}
		r->used--;
	}
	
	return 0;
}

static int cgi_response_parse(server *srv, connection *con, plugin_data *p, buffer *in, int eol) {
	char *ns;
	const char *s;
	int line = 0;
	
	UNUSED(srv);
	
	buffer_copy_string_buffer(p->parse_response, in);
	
	for (s = p->parse_response->ptr; 
	     NULL != (ns = (eol == EOL_RN ? strstr(s, "\r\n") : strchr(s, '\n'))); 
	     s = ns + (eol == EOL_RN ? 2 : 1), line++) {
		const char *key, *value;
		int key_len;
		data_string *ds;
		
		ns[0] = '\0';
		
		if (line == 0 && 
		    0 == strncmp(s, "HTTP/1.", 7)) {
			/* non-parsed header ... we parse them anyway */
			
			if ((s[7] == '1' ||
			     s[7] == '0') &&
			    s[8] == ' ') {
				int status;
				/* after the space should be a status code for us */
				
				status = strtol(s+9, NULL, 10);
				
				if (con->http_status >= 100 &&
				    con->http_status < 1000) {
					/* we expected 3 digits and didn't got them */
					con->parsed_response |= HTTP_STATUS;
					con->http_status = status;
				}
			}
		} else {
		
			key = s;
			if (NULL == (value = strchr(s, ':'))) {
				/* we expect: "<key>: <value>\r\n" */
				continue;
			}
			
			key_len = value - key;
			value += 1;
			
			/* skip LWS */
			while (*value == ' ' || *value == '\t') value++;
			
			if (NULL == (ds = (data_string *)array_get_unused_element(con->response.headers, TYPE_STRING))) {
				ds = data_response_init();
			}
			buffer_copy_string_len(ds->key, key, key_len);
			buffer_copy_string(ds->value, value);
			
			array_insert_unique(con->response.headers, (data_unset *)ds);
			
			switch(key_len) {
			case 4:
				if (0 == strncasecmp(key, "Date", key_len)) {
					con->parsed_response |= HTTP_DATE;
				}
				break;
			case 6:
				if (0 == strncasecmp(key, "Status", key_len)) {
					con->http_status = strtol(value, NULL, 10);
					con->parsed_response |= HTTP_STATUS;
				}
				break;
			case 8:
				if (0 == strncasecmp(key, "Location", key_len)) {
					con->parsed_response |= HTTP_LOCATION;
				}
				break;
			case 10:
				if (0 == strncasecmp(key, "Connection", key_len)) {
					con->response.keep_alive = (0 == strcasecmp(value, "Keep-Alive")) ? 1 : 0;
					con->parsed_response |= HTTP_CONNECTION;
				}
				break;
			case 14:
				if (0 == strncasecmp(key, "Content-Length", key_len)) {
					con->response.content_length = strtol(value, NULL, 10);
					con->parsed_response |= HTTP_CONTENT_LENGTH;
				}
				break;
			default:
				break;
			}
		}
	}
	
	/* CGI/1.1 rev 03 - 7.2.1.2 */
	if ((con->parsed_response & HTTP_LOCATION) &&
	    !(con->parsed_response & HTTP_STATUS)) {
		con->http_status = 302;
	}
	
	return 0;
}


static int cgi_demux_response(server *srv, handler_ctx *hctx) {
	plugin_data *p    = hctx->plugin_data;
	connection  *con  = hctx->remote_conn;
	
	while(1) {
		int n;
		
		buffer_prepare_copy(hctx->response, 1024);
		if (-1 == (n = read(hctx->read_fd->fd, hctx->response->ptr, hctx->response->size - 1))) {
			if (errno == EAGAIN || errno == EINTR) {
				/* would block, wait for signal */
				return FDEVENT_HANDLED_NOT_FINISHED;
			}
			/* error */
			log_error_write(srv, __FILE__, __LINE__, "sdd", strerror(errno), con->fd->fd, hctx->read_fd->fd);
			return FDEVENT_HANDLED_ERROR;
		}
		
		if (n == 0) {
			/* read finished */
			
			con->file_finished = 1;
			
			/* send final chunk */
			http_chunk_append_mem(srv, con, NULL, 0);
			joblist_append(srv, con);
			
			return FDEVENT_HANDLED_FINISHED;
		}
		
		hctx->response->ptr[n] = '\0';
		hctx->response->used = n+1;
		
		/* split header from body */
		
		if (con->file_started == 0) {
			char *c;
			int in_header = 0;
			int header_end = 0;
			int cp, eol = EOL_UNSET;
			size_t used = 0;
			
			buffer_append_string_buffer(hctx->response_header, hctx->response);
			
			/* nph (non-parsed headers) */
			if (0 == strncmp(hctx->response_header->ptr, "HTTP/1.", 7)) in_header = 1;
			
			/* search for the \r\n\r\n or \n\n in the string */
			for (c = hctx->response_header->ptr, cp = 0, used = hctx->response_header->used - 1; used; c++, cp++, used--) {
				if (*c == ':') in_header = 1;
				else if (*c == '\n') {
					if (in_header == 0) {
						/* got a response without a response header */
						
						c = NULL;
						header_end = 1;
						break;
					}
					
					if (eol == EOL_UNSET) eol = EOL_N;
					
					if (*(c+1) == '\n') {
						header_end = 1;
						break;
					}
					
				} else if (used > 1 && *c == '\r' && *(c+1) == '\n') {
					if (in_header == 0) {
						/* got a response without a response header */
						
						c = NULL;
						header_end = 1;
						break;
					}
					
					if (eol == EOL_UNSET) eol = EOL_RN;
					
					if (used > 3 &&
					    *(c+2) == '\r' && 
					    *(c+3) == '\n') {
						header_end = 1;
						break;
					}
					
					/* skip the \n */
					c++;
					cp++;
					used--;
				}
			}
			
			if (header_end) {
				if (c == NULL) {
					/* no header, but a body */
					
					if (con->request.http_version == HTTP_VERSION_1_1) {
						con->response.transfer_encoding = HTTP_TRANSFER_ENCODING_CHUNKED;
					}
					
					http_chunk_append_mem(srv, con, hctx->response_header->ptr, hctx->response_header->used);
					joblist_append(srv, con);
				} else {
					size_t hlen = c - hctx->response_header->ptr + (eol == EOL_RN ? 4 : 2);
					size_t blen = hctx->response_header->used - hlen - 1;
				
					/* a small hack: terminate after at the second \r */
					hctx->response_header->used = hlen + 1 - (eol == EOL_RN ? 2 : 1);
					hctx->response_header->ptr[hlen - (eol == EOL_RN ? 2 : 1)] = '\0';
				
					/* parse the response header */
					cgi_response_parse(srv, con, p, hctx->response_header, eol);
					
					/* enable chunked-transfer-encoding */
					if (con->request.http_version == HTTP_VERSION_1_1 &&
					    !(con->parsed_response & HTTP_CONTENT_LENGTH)) {
						con->response.transfer_encoding = HTTP_TRANSFER_ENCODING_CHUNKED;
					}
					
					if ((hctx->response->used != hlen) && blen > 0) {
						http_chunk_append_mem(srv, con, c + (eol == EOL_RN ? 4: 2), blen + 1);
						joblist_append(srv, con);
					}
				}
				
				con->file_started = 1;
			}
		} else {
			http_chunk_append_mem(srv, con, hctx->response->ptr, hctx->response->used);
			joblist_append(srv, con);
		}
		
#if 0		
		log_error_write(srv, __FILE__, __LINE__, "ddss", con->fd->fd, hctx->read_fd->fd, connection_get_state(con->state), b->ptr);
#endif
	}
	
	return FDEVENT_HANDLED_NOT_FINISHED;
}

static void cgi_cleanup(server *srv, handler_ctx *hctx) {
	if (!hctx) return;

	if (hctx->read_fd->fd != -1) {
		/* close connection to the cgi-script */
		fdevent_event_del(srv->ev, hctx->read_fd);
		fdevent_unregister(srv->ev, hctx->read_fd);

		file_descr_reset(hctx->read_fd);
	}

	if (hctx->write_fd->fd != -1) {
		/* close connection to the cgi-script */
		fdevent_event_del(srv->ev, hctx->write_fd);
		fdevent_unregister(srv->ev, hctx->write_fd);
	
		file_descr_reset(hctx->read_fd);
	}
}

static handler_t cgi_connection_close(server *srv, handler_ctx *hctx) {
	int status;
	pid_t pid;
	plugin_data *p;
	connection  *con;
	
	if (NULL == hctx) return HANDLER_GO_ON;
	
	p    = hctx->plugin_data;
	con  = hctx->remote_conn;
	
	if (con->mode != p->id) return HANDLER_GO_ON;

#ifndef __WIN32
	
#if 0
	log_error_write(srv, __FILE__, __LINE__, "sdd", 
			"emergency exit: cgi", 
			con->fd->fd,
			hctx->read_fd->fd);
#endif
	
	/* the connection to the browser went away, but we still have a connection
	 * to the CGI script 
	 *
	 * close cgi-connection
	 */
	
	cgi_cleanup(srv, hctx);
	
	pid = hctx->pid;
	
	con->plugin_ctx[p->id] = NULL;
	
	/* is this a good idea ? */
	cgi_handler_ctx_free(hctx);
	
	/* if waitpid hasn't been called by response.c yet, do it here */
	if (pid) {
		/* check if the CGI-script is already gone */
		switch(waitpid(pid, &status, WNOHANG)) {
		case 0:
			/* not finished yet */
#if 0
			log_error_write(srv, __FILE__, __LINE__, "sd", "(debug) child isn't done yet, pid:", pid);
#endif
			break;
		case -1:
			/* */
			if (errno == EINTR) break;
			
			/* 
			 * errno == ECHILD happens if _subrequest catches the process-status before 
			 * we have read the response of the cgi process
			 * 
			 * -> catch status
			 * -> WAIT_FOR_EVENT
			 * -> read response
			 * -> we get here with waitpid == ECHILD
			 * 
			 */
			if (errno == ECHILD) return HANDLER_FINISHED;
			
			log_error_write(srv, __FILE__, __LINE__, "ss", "waitpid failed: ", strerror(errno));
			return HANDLER_ERROR;
		default:
			if (WIFEXITED(status)) {
#if 0
				log_error_write(srv, __FILE__, __LINE__, "sd", "(debug) cgi exited fine, pid:", pid);
#endif
				pid = 0;
				
				return HANDLER_FINISHED;
			} else {
				log_error_write(srv, __FILE__, __LINE__, "sd", "cgi died, pid:", pid);
				pid = 0;
				return HANDLER_FINISHED;
			}
		}
		
	
		kill(pid, SIGTERM);
		
		/* cgi-script is still alive, queue the PID for removal */
		cgi_pid_add(srv, p, pid);
	}
#endif	
	return HANDLER_FINISHED;
}

static handler_t cgi_connection_close_callback(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;
	
	return cgi_connection_close(srv, con->plugin_ctx[p->id]);
}


static handler_t cgi_handle_fdevent(void *s, void *ctx, int revents) {
	server      *srv  = (server *)s;
	handler_ctx *hctx = ctx;
	connection  *con  = hctx->remote_conn;
	
	if (hctx->read_fd->fd == -1) {
		log_error_write(srv, __FILE__, __LINE__, "ddss", con->fd->fd, hctx->read_fd->fd, connection_get_state(con->state), "invalid cgi-fd");
		
		return HANDLER_ERROR;
	}
	
	if (revents & FDEVENT_IN) {
		switch (cgi_demux_response(srv, hctx)) {
		case FDEVENT_HANDLED_NOT_FINISHED:
			break;
		case FDEVENT_HANDLED_FINISHED:
			/* we are done */
			
#if 0
			log_error_write(srv, __FILE__, __LINE__, "ddss", con->fd->fd, hctx->read_fd->fd, connection_get_state(con->state), "finished");
#endif
			
			break;
		case FDEVENT_HANDLED_ERROR:
			connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);
			con->http_status = 500;
			con->mode = DIRECT;
			
			log_error_write(srv, __FILE__, __LINE__, "s", "demuxer failed: ");
			break;
		}
	}
	
	if (revents & FDEVENT_OUT) {
		network_t ret;
		/* do we have to send something from the read-queue ? */
		
		/* check the length of the read_queue, 
		 * how much we have already forwarded to cgi and 
		 * how much still have to send */

		
		switch(ret = network_write_chunkqueue(srv, hctx->write_fd, hctx->write_queue)) {
		case NETWORK_QUEUE_EMPTY:
			if (con->request.content_finished) {
				fdevent_event_del(srv->ev, hctx->write_fd);
				fdevent_unregister(srv->ev, hctx->write_fd);
				close(hctx->write_fd->fd);
				hctx->write_fd->fd = -1;
				
				fdevent_event_add(srv->ev, hctx->read_fd, FDEVENT_IN);
				/* wait for input */
			}
			break;
		case NETWORK_OK:
			/* not finished yet, queue not empty yet, wait for event */
			break;
		case NETWORK_ERROR: /* error on our side */
			log_error_write(srv, __FILE__, __LINE__, "sd",
					"connection closed: write failed on fd", hctx->write_fd->fd);
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
	}
	
	/* perhaps this issue is already handled */
	if (revents & FDEVENT_HUP) {
		/* check if we still have a unfinished header package which is a body in reality */
		if (con->file_started == 0 &&
		    hctx->response_header->used) {
			con->file_started = 1;
			http_chunk_append_mem(srv, con, hctx->response_header->ptr, hctx->response_header->used);
			joblist_append(srv, con);
		}
		
		if (con->file_finished == 0) {
			http_chunk_append_mem(srv, con, NULL, 0);
			joblist_append(srv, con);
		}
		
		con->file_finished = 1;
		
		if (chunkqueue_is_empty(con->write_queue)) {
			/* there is nothing left to write */
			connection_set_state(srv, con, CON_STATE_RESPONSE_END);
		} else {
			/* used the write-handler to finish the request on demand */
			
		}
		
# if 0
		log_error_write(srv, __FILE__, __LINE__, "sddd", "got HUP from cgi", con->fd->fd, hctx->read_fd->fd, revents);
# endif
		
		/* rtsigs didn't liked the close */
		cgi_connection_close(srv, hctx);
	} else if (revents & FDEVENT_ERR) {
		con->file_finished = 1;
		
		/* kill all connections to the cgi process */
		cgi_connection_close(srv, hctx);
#if 1
		log_error_write(srv, __FILE__, __LINE__, "s", "cgi-FDEVENT_ERR");
#endif			
		return HANDLER_ERROR;
	}
	
	return HANDLER_FINISHED;
}


static int cgi_env_add(char_array *env, const char *key, size_t key_len, const char *val) {
	int val_len;
	char *dst;
	
	if (!key || !val) return -1;
	
	val_len = strlen(val);
	
	dst = malloc(key_len + val_len + 3);
	memcpy(dst, key, key_len);
	dst[key_len] = '=';
	/* add the \0 from the value */
	memcpy(dst + key_len + 1, val, val_len + 1);
	
	if (env->size == 0) {
		env->size = 16;
		env->ptr = malloc(env->size * sizeof(*env->ptr));
	} else if (env->size == env->used) {
		env->size += 16;
		env->ptr = realloc(env->ptr, env->size * sizeof(*env->ptr));
	}
	
	env->ptr[env->used++] = dst;
	
	return 0;
}

static int cgi_create_env(server *srv, connection *con, plugin_data *p, buffer *cgi_handler) {
	pid_t pid;
	
#ifdef HAVE_IPV6
	char b2[INET6_ADDRSTRLEN + 1];
#endif
	
	int to_cgi_fds[2];
	int from_cgi_fds[2];
	struct stat st;
	
#ifndef __WIN32	
	
	if (cgi_handler->used > 1) {
		/* stat the exec file */
		if (-1 == (stat(cgi_handler->ptr, &st))) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", 
					"stat for cgi-handler", cgi_handler,
					"failed:", strerror(errno));
			return -1;
		}
	}
	
	if (pipe(to_cgi_fds)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "pipe failed:", strerror(errno));
		return -1;
	}
	
	if (pipe(from_cgi_fds)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "pipe failed:", strerror(errno));
		return -1;
	}
	
	/* fork, execve */
	switch (pid = fork()) {
	case 0: {
		/* child */
		char **args;
		int argc;
		int i = 0;
		char buf[32];
		size_t n;
		char_array env;
		char *c;
		server_socket *srv_sock = con->srv_socket;
		
		/* move stdout to from_cgi_fd[1] */
		close(STDOUT_FILENO);
		dup2(from_cgi_fds[1], STDOUT_FILENO);
		close(from_cgi_fds[1]);
		/* not needed */
		close(from_cgi_fds[0]);
		
		/* move the stdin to to_cgi_fd[0] */
		close(STDIN_FILENO);
		dup2(to_cgi_fds[0], STDIN_FILENO);
		close(to_cgi_fds[0]);
		/* not needed */
		close(to_cgi_fds[1]);
		
		/* create environment */
		env.ptr = NULL;
		env.size = 0;
		env.used = 0;
		
		cgi_env_add(&env, CONST_STR_LEN("SERVER_SOFTWARE"), PACKAGE_NAME"/"PACKAGE_VERSION);
		cgi_env_add(&env, CONST_STR_LEN("SERVER_NAME"), 
			    con->server_name->used ?
			    con->server_name->ptr :
#ifdef HAVE_IPV6
			    inet_ntop(srv_sock->addr.plain.sa_family, 
				      srv_sock->addr.plain.sa_family == AF_INET6 ? 
				      (const void *) &(srv_sock->addr.ipv6.sin6_addr) :
				      (const void *) &(srv_sock->addr.ipv4.sin_addr),
				      b2, sizeof(b2)-1)
#else
			    inet_ntoa(srv_sock->addr.ipv4.sin_addr)
#endif
			    );
		cgi_env_add(&env, CONST_STR_LEN("GATEWAY_INTERFACE"), "CGI/1.1");
		
		cgi_env_add(&env, CONST_STR_LEN("SERVER_PROTOCOL"), get_http_version_name(con->request.http_version));
		
		ltostr(buf, 
#ifdef HAVE_IPV6
			ntohs(srv_sock->addr.plain.sa_family == AF_INET6 ? srv_sock->addr.ipv6.sin6_port : srv_sock->addr.ipv4.sin_port)
#else
			ntohs(srv_sock->addr.ipv4.sin_port)
#endif
			);
		cgi_env_add(&env, CONST_STR_LEN("SERVER_PORT"), buf);
		
		cgi_env_add(&env, CONST_STR_LEN("SERVER_ADDR"), 
#ifdef HAVE_IPV6
			    inet_ntop(srv_sock->addr.plain.sa_family, 
				      srv_sock->addr.plain.sa_family == AF_INET6 ? 
				      (const void *) &(srv_sock->addr.ipv6.sin6_addr) :
				      (const void *) &(srv_sock->addr.ipv4.sin_addr),
				      b2, sizeof(b2)-1)
#else
			    inet_ntoa(srv_sock->addr.ipv4.sin_addr)
#endif
			    );
		
		cgi_env_add(&env, CONST_STR_LEN("REQUEST_METHOD"), con->request.http_method_name->ptr);
		if (con->request.pathinfo->used) {
			cgi_env_add(&env, CONST_STR_LEN("PATH_INFO"), con->request.pathinfo->ptr);
		}
		cgi_env_add(&env, CONST_STR_LEN("REDIRECT_STATUS"), "200");
		cgi_env_add(&env, CONST_STR_LEN("QUERY_STRING"), con->uri.query->used ? con->uri.query->ptr : "");
		cgi_env_add(&env, CONST_STR_LEN("REQUEST_URI"), con->request.orig_uri->used ? con->request.orig_uri->ptr : "");
		
		
		cgi_env_add(&env, CONST_STR_LEN("REMOTE_ADDR"), 
#ifdef HAVE_IPV6
			    inet_ntop(con->dst_addr.plain.sa_family, 
				      con->dst_addr.plain.sa_family == AF_INET6 ? 
				      (const void *) &(con->dst_addr.ipv6.sin6_addr) :
				      (const void *) &(con->dst_addr.ipv4.sin_addr),
				      b2, sizeof(b2)-1)
#else
			    inet_ntoa(con->dst_addr.ipv4.sin_addr)
#endif
			    );

		ltostr(buf, 
#ifdef HAVE_IPV6
			ntohs(con->dst_addr.plain.sa_family == AF_INET6 ? con->dst_addr.ipv6.sin6_port : con->dst_addr.ipv4.sin_port)
#else
			ntohs(con->dst_addr.ipv4.sin_port)
#endif
			);
		cgi_env_add(&env, CONST_STR_LEN("REMOTE_PORT"), buf);
		
		if (con->authed_user->used) {
			cgi_env_add(&env, CONST_STR_LEN("REMOTE_USER"),
				    con->authed_user->ptr);
		}
		
		/* request.content_length < SSIZE_MAX, see request.c */
		ltostr(buf, con->request.content_length);
		cgi_env_add(&env, CONST_STR_LEN("CONTENT_LENGTH"), buf);
		cgi_env_add(&env, CONST_STR_LEN("SCRIPT_FILENAME"), con->physical.path->ptr);
		cgi_env_add(&env, CONST_STR_LEN("SCRIPT_NAME"), con->uri.path->ptr);
		
		/* for valgrind */
		cgi_env_add(&env, CONST_STR_LEN("LD_PRELOAD"), getenv("LD_PRELOAD"));
		cgi_env_add(&env, CONST_STR_LEN("LD_LIBRARY_PATH"), getenv("LD_LIBRARY_PATH"));
#ifdef __CYGWIN__
		/* CYGWIN needs SYSTEMROOT */
		cgi_env_add(&env, CONST_STR_LEN("SYSTEMROOT"), getenv("SYSTEMROOT"));
#endif
		
		for (n = 0; n < con->request.headers->used; n++) {
			data_string *ds;
			
			ds = (data_string *)con->request.headers->data[n];
			
			if (ds->value->used && ds->key->used) {
				size_t j;
				
				buffer_reset(p->tmp_buf);
				
				if (0 != strcasecmp(ds->key->ptr, "CONTENT-TYPE")) {
					buffer_copy_string(p->tmp_buf, "HTTP_");
					p->tmp_buf->used--; /* strip \0 after HTTP_ */
				}
				
				buffer_prepare_append(p->tmp_buf, ds->key->used + 2);
				
				for (j = 0; j < ds->key->used - 1; j++) {
					p->tmp_buf->ptr[p->tmp_buf->used++] = 
						isalpha((unsigned char)ds->key->ptr[j]) ? 
						toupper((unsigned char)ds->key->ptr[j]) : '_';
				}
				p->tmp_buf->ptr[p->tmp_buf->used++] = '\0';
				
				cgi_env_add(&env, CONST_BUF_LEN(p->tmp_buf), ds->value->ptr);
			}
		}
		
		for (n = 0; n < con->environment->used; n++) {
			data_string *ds;
			
			ds = (data_string *)con->environment->data[n];
			
			if (ds->value->used && ds->key->used) {
				size_t j;
				
				buffer_reset(p->tmp_buf);
				
				buffer_prepare_append(p->tmp_buf, ds->key->used + 2);
				
				for (j = 0; j < ds->key->used - 1; j++) {
					p->tmp_buf->ptr[p->tmp_buf->used++] = 
						isalpha((unsigned char)ds->key->ptr[j]) ? 
						toupper((unsigned char)ds->key->ptr[j]) : '_';
				}
				p->tmp_buf->ptr[p->tmp_buf->used++] = '\0';
				
				cgi_env_add(&env, CONST_BUF_LEN(p->tmp_buf), ds->value->ptr);
			}
		}
		
		if (env.size == env.used) {
			env.size += 16;
			env.ptr = realloc(env.ptr, env.size * sizeof(*env.ptr));
		}
		
		env.ptr[env.used] = NULL;
		
		/* set up args */
		argc = 3;
		args = malloc(sizeof(*args) * argc);
		i = 0;
		
		if (cgi_handler->used > 1) {
			args[i++] = cgi_handler->ptr;
		}
		args[i++] = con->physical.path->ptr;
		args[i++] = NULL;

		/* search for the last / */
		if (NULL != (c = strrchr(con->physical.path->ptr, '/'))) {
			*c = '\0';
			
			/* change to the physical directory */
			if (-1 == chdir(con->physical.path->ptr)) {
				log_error_write(srv, __FILE__, __LINE__, "ssb", "chdir failed:", strerror(errno), con->physical.path);
			}
			*c = '/';
		}

		/* we don't need the client socket */
		for (i = 3; i < 256; i++) {
			if (i != srv->log_error_fd) close(i);
		}
		
		/* exec the cgi */
		execve(args[0], args, env.ptr);
		
		log_error_write(srv, __FILE__, __LINE__, "sss", "CGI failed:", strerror(errno), args[0]);
		
		/* */
		SEGFAULT();
		break;
	}
	case -1:
		/* error */
		log_error_write(srv, __FILE__, __LINE__, "ss", "fork failed:", strerror(errno));
		break;
	default: {
		handler_ctx *hctx;
		/* father */
	
		



		if (con->request.content->used) {
			write(to_cgi_fds[1], con->request.content->ptr, con->request.content_length);
		}
		
		close(from_cgi_fds[1]);
		
		close(to_cgi_fds[0]);
		
		/* register PID and wait for them asyncronously */
		con->mode = p->id;
		buffer_reset(con->physical.path);
		
		hctx = cgi_handler_ctx_init();
		
		hctx->remote_conn = con;
		hctx->plugin_data = p;
		hctx->pid = pid;


		/* reading fd */
		
		hctx->read_fd->fd = from_cgi_fds[0];

		fdevent_register(srv->ev, hctx->read_fd, cgi_handle_fdevent, hctx);
		
		if (-1 == fdevent_fcntl_set(srv->ev, hctx->read_fd)) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "fcntl failed: ", strerror(errno));

			cgi_cleanup(srv, hctx);
			
			cgi_handler_ctx_free(hctx);
						
			return -1;
		}

		/* writing fd */
		if (con->request.content_length) {
			/* we have to send the request-body to the cgi */
	
			hctx->write_fd->fd = to_cgi_fds[1];

			fdevent_register(srv->ev, hctx->write_fd, cgi_handle_fdevent, hctx);
			fdevent_event_add(srv->ev, hctx->write_fd, FDEVENT_OUT);
		
			if (-1 == fdevent_fcntl_set(srv->ev, hctx->write_fd)) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "fcntl failed: ", strerror(errno));

				cgi_cleanup(srv, hctx);
				
				cgi_handler_ctx_free(hctx);
			
				return -1;
			}
		} else {
			close(to_cgi_fds[1]);
			/* only accept incoming data after we have sent all our data */
			fdevent_event_add(srv->ev, hctx->read_fd, FDEVENT_IN);
		}
		
		con->plugin_ctx[p->id] = hctx;

		break;
	}
	}
	
	return 0;
#else
	return -1;
#endif
}

#define PATCH(x) \
	p->conf.x = s->x;
static int mod_cgi_patch_connection(server *srv, connection *con, plugin_data *p, const char *stage, size_t stage_len) {
	size_t i, j;
	
	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		plugin_config *s = p->config_storage[i];
		
		/* not our stage */
		if (!buffer_is_equal_string(dc->comp_key, stage, stage_len)) continue;
		
		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;
		
		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];
			
			if (buffer_is_equal_string(du->key, CONST_STR_LEN("cgi.assign"))) {
				PATCH(cgi);
			}
		}
	}
	
	return 0;
}

static int mod_cgi_setup_connection(server *srv, connection *con, plugin_data *p) {
	plugin_config *s = p->config_storage[0];
	UNUSED(srv);
	UNUSED(con);
		
	PATCH(cgi);
	
	return 0;
}
#undef PATCH

URIHANDLER_FUNC(cgi_is_handled) {
	size_t k, s_len, i;
	plugin_data *p = p_d;
	buffer *fn = con->physical.path;
	
	if (fn->used == 0) return HANDLER_ERROR;
	
	mod_cgi_setup_connection(srv, con, p);
	for (i = 0; i < srv->config_patches->used; i++) {
		buffer *patch = srv->config_patches->ptr[i];
		
		mod_cgi_patch_connection(srv, con, p, CONST_BUF_LEN(patch));
	}
	
	s_len = fn->used - 1;
	
	for (k = 0; k < p->conf.cgi->used; k++) {
		data_string *ds = (data_string *)p->conf.cgi->data[k];
		size_t ct_len = ds->key->used - 1;
		
		if (ds->key->used == 0) continue;
		if (s_len < ct_len) continue;
		
		if (0 == strncmp(fn->ptr + s_len - ct_len, ds->key->ptr, ct_len)) {
			if (cgi_create_env(srv, con, p, ds->value)) {
				con->http_status = 500;
				
				buffer_reset(con->physical.path);
			}
			
			return HANDLER_FINISHED;
		}
	}
	
	return HANDLER_GO_ON;
}

TRIGGER_FUNC(cgi_trigger) {
	plugin_data *p = p_d;
	size_t ndx;
	/* the trigger handle only cares about lonely PID which we have to wait for */
#ifndef __WIN32

	for (ndx = 0; ndx < p->cgi_pid.used; ndx++) {
		int status;
		
		switch(waitpid(p->cgi_pid.ptr[ndx], &status, WNOHANG)) {
		case 0:
			/* not finished yet */
#if 0
			log_error_write(srv, __FILE__, __LINE__, "sd", "(debug) child isn't done yet, pid:", p->cgi_pid.ptr[ndx]);
#endif
			break;
		case -1:
			log_error_write(srv, __FILE__, __LINE__, "ss", "waitpid failed: ", strerror(errno));
			
			return HANDLER_ERROR;
		default:

			if (WIFEXITED(status)) {
#if 0
				log_error_write(srv, __FILE__, __LINE__, "sd", "(debug) cgi exited fine, pid:", p->cgi_pid.ptr[ndx]);
#endif
			} else {
				log_error_write(srv, __FILE__, __LINE__, "s", "cgi died ?");
			}
			
			cgi_pid_del(srv, p, p->cgi_pid.ptr[ndx]);
			/* del modified the buffer structure 
			 * and copies the last entry to the current one
			 * -> recheck the current index
			 */
			ndx--;
		}
	}
#endif	
	return HANDLER_GO_ON;
}

SUBREQUEST_FUNC(mod_cgi_fetch_post_data) {
	plugin_data *p = p_d;
	handler_ctx *hctx = con->plugin_ctx[p->id];
	chunkqueue *cq;
	chunk *c;
	
	if (con->mode != p->id) return HANDLER_GO_ON;
	if (NULL == hctx) return HANDLER_GO_ON;

#if 0
	fprintf(stderr, "%s.%d: fetching data: %d / %d\n", __FILE__, __LINE__, hctx->post_data_fetched, con->request.content_length);

#endif
	cq = con->read_queue;

	for (c = cq->first; c && (hctx->post_data_fetched != con->request.content_length); c = cq->first) {
		off_t weWant, weHave, toRead;
			
		weWant = con->request.content_length - hctx->post_data_fetched;
		/* without the terminating \0 */
			
		assert(c->data.mem->used);
			
		weHave = c->data.mem->used - c->offset - 1;
				
		toRead = weHave > weWant ? weWant : weHave;
			
		chunkqueue_append_mem(hctx->write_queue, c->data.mem->ptr + c->offset, toRead + 1);
			
		c->offset += toRead;
		hctx->post_data_fetched += toRead;
		
		chunkqueue_remove_empty_chunks(cq);
	}
		
	/* Content is ready */
	if (hctx->post_data_fetched == con->request.content_length) {
		con->request.content_finished = 1;
	}

	return HANDLER_FINISHED;
}


SUBREQUEST_FUNC(mod_cgi_handle_subrequest) {
	int status;
	plugin_data *p = p_d;
	handler_ctx *hctx = con->plugin_ctx[p->id];
	
	if (con->mode != p->id) return HANDLER_GO_ON;
	if (NULL == hctx) return HANDLER_GO_ON;
	
#if 0
	log_error_write(srv, __FILE__, __LINE__, "sdd", "subrequest, pid =", hctx, hctx->pid);
#endif	
	if (hctx->pid == 0) return HANDLER_FINISHED;
#ifndef __WIN32	
	switch(waitpid(hctx->pid, &status, WNOHANG)) {
	case 0:
		/* not finished yet */
		if (con->file_started) {
			return HANDLER_GO_ON;
		} else {
			return HANDLER_WAIT_FOR_EVENT;
		}
	case -1:
		if (errno == EINTR) return HANDLER_WAIT_FOR_EVENT;
		
		if (errno == ECHILD && con->file_started == 0) {
			/*
			 * second round but still not response 
			 */
			return HANDLER_WAIT_FOR_EVENT; 
		}
		
		log_error_write(srv, __FILE__, __LINE__, "ss", "waitpid failed: ", strerror(errno));
		con->mode = DIRECT;
		con->http_status = 500;
		
		hctx->pid = 0;

		cgi_cleanup(srv, hctx);
		
		cgi_handler_ctx_free(hctx);
		
		con->plugin_ctx[p->id] = NULL;
		
		return HANDLER_FINISHED;
	default:
		/* cgi process exited cleanly 
		 * 
		 * check if we already got the response 
		 */
		
		if (!con->file_started) return HANDLER_WAIT_FOR_EVENT;
		
		if (WIFEXITED(status)) {
			/* nothing */
		} else {
			log_error_write(srv, __FILE__, __LINE__, "s", "cgi died ?");
			
			con->mode = DIRECT;
			con->http_status = 500;
			
		}
		
		hctx->pid = 0;
		
		cgi_cleanup(srv, hctx);
				
		cgi_handler_ctx_free(hctx);
		
		con->plugin_ctx[p->id] = NULL;
		return HANDLER_FINISHED;
	}
#else
	return HANDLER_ERROR;
#endif
}


int mod_cgi_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("cgi");

	p->handle_connection_close = cgi_connection_close_callback;
	p->handle_subrequest_start = cgi_is_handled;
	p->handle_subrequest = mod_cgi_handle_subrequest;

	p->handle_fetch_post_data = mod_cgi_fetch_post_data;
	p->handle_trigger = cgi_trigger;
	p->init           = mod_cgi_init;
	p->cleanup        = mod_cgi_free;
	p->set_defaults   = mod_fastcgi_set_defaults;
	
	p->data        = NULL;
	
	return 0;
}
