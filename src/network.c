#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "file_cache.h"
#include "connections.h"
#include "plugin.h"
#include "joblist.h"

#include "network_backends.h"
#include "sys-mmap.h"
#include "sys-socket.h"
#include "file_descr_funcs.h"

#ifdef USE_OPENSSL
# include <openssl/ssl.h> 
# include <openssl/err.h> 
# include <openssl/rand.h> 
#endif

handler_t network_server_handle_fdevent(void *s, void *context, int revents) {
	server     *srv = (server *)s;
	server_socket *srv_socket = (server_socket *)context;
	connection *con;
	
	UNUSED(context);
	
	if (revents != FDEVENT_IN) {
		log_error_write(srv, __FILE__, __LINE__, "sdd", 
				"strange event for server socket",
				srv_socket->fd->fd,
				revents);
		return HANDLER_ERROR;
	}
	
	while (NULL != (con = connection_accept(srv, srv_socket))) {
		handler_t r;
		
		if (srv_socket->is_ssl) {
#ifdef USE_OPENSSL
			con->fd->write_func = network_write_chunkqueue_openssl;
			con->fd->read_func = network_read_chunkqueue_openssl;
#endif
		} else {
		/* dispatch call */
#if defined USE_LINUX_SENDFILE
			con->fd->write_func = network_write_chunkqueue_linuxsendfile;
#elif defined USE_FREEBSD_SENDFILE
			con->fd->write_func = network_write_chunkqueue_freebsdsendfile;
#elif defined USE_SOLARIS_SENDFILEV
			con->fd->write_func = network_write_chunkqueue_solarissendfilev;
#elif defined USE_WRITEV
			con->fd->write_func = network_write_chunkqueue_writev;
#else
			con->fd->write_func = network_write_chunkqueue_write;
#endif
			con->fd->read_func = network_read_chunkqueue_write;
		}

		connection_state_machine(srv, con);
		
		switch(r = plugins_call_handle_joblist(srv, con)) {
		case HANDLER_FINISHED:
		case HANDLER_GO_ON:
			break;
		default:
			log_error_write(srv, __FILE__, __LINE__, "d", r);
			break;
		}
	}
	return HANDLER_GO_ON;
}

int network_server_init(server *srv, buffer *host_token, specific_config *s) {
	int val;
	socklen_t addr_len;
	server_socket *srv_socket;
	char *sp;
	unsigned int port = 0;
	const char *host;
	buffer *b;
	
#ifdef SO_ACCEPTFILTER
	struct accept_filter_arg afa;
#endif

#ifdef __WIN32
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;
	 
	wVersionRequested = MAKEWORD( 2, 2 );
	 
	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 ) {
		    /* Tell the user that we could not find a usable */
		    /* WinSock DLL.                                  */
		    return -1;
	}
#endif
	
	srv_socket = calloc(1, sizeof(*srv_socket));
	srv_socket->fd = file_descr_init();
	
	srv_socket->srv_token = buffer_init();
	buffer_copy_string_buffer(srv_socket->srv_token, host_token);
	
	b = buffer_init();
	buffer_copy_string_buffer(b, host_token);
	
	/* ipv4:port 
	 * [ipv6]:port
	 */
	if (NULL == (sp = strrchr(b->ptr, ':'))) {
		log_error_write(srv, __FILE__, __LINE__, "sb", "value of $SERVER[\"socket\"] has to be \"ip:port\".", b);
		
		return -1;
	}
	
	host = b->ptr;
	
	/* check for [ and ] */
	if (b->ptr[0] == '[' && *(sp-1) == ']') {
		*(sp-1) = '\0';
		host++;
		
		s->use_ipv6 = 1;
	}
	
	*(sp++) = '\0';
	
	port = strtol(sp, NULL, 10);
	
	if (port == 0 || port > 65535) {
		log_error_write(srv, __FILE__, __LINE__, "sd", "port out of range:", port);
	
		return -1;
	}
	
	if (*host == '\0') host = NULL;
	
#ifdef HAVE_IPV6
	if (s->use_ipv6) {
		srv_socket->addr.plain.sa_family = AF_INET6;
		
		if (-1 == (srv_socket->fd->fd = socket(srv_socket->addr.plain.sa_family, SOCK_STREAM, IPPROTO_TCP))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "socket failed:", strerror(errno));
			return -1;
		}
		srv_socket->use_ipv6 = 1;
	}
#endif
				
	if (srv_socket->fd->fd == -1) {
		srv_socket->addr.plain.sa_family = AF_INET;
		if (-1 == (srv_socket->fd->fd = socket(srv_socket->addr.plain.sa_family, SOCK_STREAM, IPPROTO_TCP))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "socket failed:", strerror(errno));
			return -1;
		}
	}
	
	/* */
	srv->cur_fds = srv_socket->fd->fd;
	
	val = 1;
	if (setsockopt(srv_socket->fd->fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "socketsockopt failed:", strerror(errno));
		return -1;
	}
	
	if (-1 == fdevent_fcntl_set(srv->ev, srv_socket->fd)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "fcntl failed:", strerror(errno));
		return -1;
	}
	
	switch(srv_socket->addr.plain.sa_family) {
#ifdef HAVE_IPV6
	case AF_INET6:
		memset(&srv_socket->addr, 0, sizeof(struct sockaddr_in6));
		srv_socket->addr.ipv6.sin6_family = AF_INET6;
		if (host == NULL) {
			srv_socket->addr.ipv6.sin6_addr = in6addr_any;
		} else {
			struct addrinfo hints, *res;
			int r;
			
			memset(&hints, 0, sizeof(hints));
			
			hints.ai_family   = AF_INET6;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;
			
			if (0 != (r = getaddrinfo(host, NULL, &hints, &res))) {
				log_error_write(srv, __FILE__, __LINE__, 
						"sssss", "getaddrinfo failed: ", 
						gai_strerror(r), "'", host, "'");
				
				return -1;
			}
			
			memcpy(&(srv_socket->addr), res->ai_addr, res->ai_addrlen);
			
			freeaddrinfo(res);
		}
		srv_socket->addr.ipv6.sin6_port = htons(port);
		addr_len = sizeof(struct sockaddr_in6);
		break;
#endif
	case AF_INET:
		memset(&srv_socket->addr, 0, sizeof(struct sockaddr_in));
		srv_socket->addr.ipv4.sin_family = AF_INET;
		if (host == NULL) {
			srv_socket->addr.ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
		} else {
			struct hostent *he;
			if (NULL == (he = gethostbyname(host))) {
				log_error_write(srv, __FILE__, __LINE__, 
						"sss", "gethostbyname failed: ", 
						hstrerror(h_errno), host);
				return -1;
			}
			
			if (he->h_addrtype != AF_INET) {
				log_error_write(srv, __FILE__, __LINE__, "sd", "addr-type != AF_INET: ", he->h_addrtype);
				return -1;
			}
			
			if (he->h_length != sizeof(struct in_addr)) {
				log_error_write(srv, __FILE__, __LINE__, "sd", "addr-length != sizeof(in_addr): ", he->h_length);
				return -1;
			}
			
			memcpy(&(srv_socket->addr.ipv4.sin_addr.s_addr), he->h_addr_list[0], he->h_length);
		}
		srv_socket->addr.ipv4.sin_port = htons(port);
		
		addr_len = sizeof(struct sockaddr_in);
		
		break;
	default:
		addr_len = 0;
		
		return -1;
	}
	
	if (0 != bind(srv_socket->fd->fd, (struct sockaddr *) &(srv_socket->addr), addr_len)) {
		log_error_write(srv, __FILE__, __LINE__, "sds", "can't bind to port", port, strerror(errno));
		return -1;
	}
	
	if (-1 == listen(srv_socket->fd->fd, 128 * 8)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "listen failed: ", strerror(errno));
		return -1;
	}
	
#ifdef SO_ACCEPTFILTER
	/*
	 * FreeBSD accf_http filter
	 *
	 */
	memset(&afa, 0, sizeof(afa));
	strcpy(afa.af_name, "httpready");
	if (setsockopt(srv_socket->fd->fd, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa)) < 0) {
		if (errno != ENOENT) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "can't set accept-filter 'httpready': ", strerror(errno));
		}
	}
#endif
	

	if (s->is_ssl) {
#ifdef USE_OPENSSL
		if (srv->ssl_is_init == 0) {
			SSL_load_error_strings();
			SSL_library_init();
			srv->ssl_is_init = 1;
			
			if (0 == RAND_status()) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
						"not enough entropy in the pool");
				return -1;
			}
		}
		
		if (NULL == (s->ssl_ctx = SSL_CTX_new(SSLv23_server_method()))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
					ERR_error_string(ERR_get_error(), NULL));
			return -1;
		}
		
		if (buffer_is_empty(s->ssl_pemfile)) {
			log_error_write(srv, __FILE__, __LINE__, "s", "ssl.pemfile has to be set");
			return -1;
		}
		
		if (!buffer_is_empty(s->ssl_ca_file)) {
			if (1 != SSL_CTX_load_verify_locations(s->ssl_ctx, s->ssl_ca_file->ptr, NULL)) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
						ERR_error_string(ERR_get_error(), NULL));
				return -1;
			}
		}
		
		if (SSL_CTX_use_certificate_file(s->ssl_ctx, s->ssl_pemfile->ptr, SSL_FILETYPE_PEM) < 0) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
					ERR_error_string(ERR_get_error(), NULL));
			return -1;
		}
		
		if (SSL_CTX_use_PrivateKey_file (s->ssl_ctx, s->ssl_pemfile->ptr, SSL_FILETYPE_PEM) < 0) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
					ERR_error_string(ERR_get_error(), NULL));
			return -1;
		}
		
		if (SSL_CTX_check_private_key(s->ssl_ctx) != 1) {
			log_error_write(srv, __FILE__, __LINE__, "sssb", "SSL:", 
					"Private key does not match the certificate public key, reason:",
					ERR_error_string(ERR_get_error(), NULL),
					s->ssl_pemfile);
			return -1;
		}
		srv_socket->ssl_ctx = s->ssl_ctx;
#else
		
		buffer_free(srv_socket->srv_token);
		free(srv_socket);
		
		buffer_free(b);
		
		log_error_write(srv, __FILE__, __LINE__, "ss", "SSL:", 
				"ssl requested but openssl support is not compiled in");
		
		return -1;
#endif
	}
	
	srv_socket->is_ssl = s->is_ssl;
	
	if (srv->srv_sockets.size == 0) {
		srv->srv_sockets.size = 4;
		srv->srv_sockets.used = 0;
		srv->srv_sockets.ptr = malloc(srv->srv_sockets.size * sizeof(server_socket));
	} else if (srv->srv_sockets.used == srv->srv_sockets.size) {
		srv->srv_sockets.size += 4;
		srv->srv_sockets.ptr = realloc(srv->srv_sockets.ptr, srv->srv_sockets.size * sizeof(server_socket));
	}
	
	srv->srv_sockets.ptr[srv->srv_sockets.used++] = srv_socket;

	buffer_free(b);
	
	return 0;
}

int network_close(server *srv) {
	size_t i;
	for (i = 0; i < srv->srv_sockets.used; i++) {
		server_socket *srv_socket = srv->srv_sockets.ptr[i];
		
		if (srv_socket->fd->fd != -1) {
			/* check if server fd are already registered */
			if (srv_socket->fd->fde_ndx != -1) {
				fdevent_event_del(srv->ev, srv_socket->fd);
				fdevent_unregister(srv->ev, srv_socket->fd);
			}
		
			close(srv_socket->fd->fd);
		}
		file_descr_free(srv_socket->fd);
		buffer_free(srv_socket->srv_token);
		
		free(srv_socket);
	}
	
	free(srv->srv_sockets.ptr);
	
	return 0;
}

int network_init(server *srv) {
	buffer *b;
	size_t i;
	
	b = buffer_init();
		
	buffer_copy_string_buffer(b, srv->srvconf.bindhost);
	buffer_append_string(b, ":");
	buffer_append_long(b, srv->srvconf.port);
	
	if (0 != network_server_init(srv, b, srv->config_storage[0])) {
		return -1;
	}
	buffer_free(b);
		
	/* check for $SERVER["socket"] */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		specific_config *s = srv->config_storage[i];
		
		/* not our stage */
		if (!buffer_is_equal_string(dc->comp_key, CONST_STR_LEN("SERVERsocket"))) continue;
		
		if (dc->cond != CONFIG_COND_EQ) {
			log_error_write(srv, __FILE__, __LINE__, "s", "only == is allowed for $SERVER[\"socket\"].");
			
			return -1;
		}
		
		if (0 != network_server_init(srv, dc->match.string, s)) {
			return -1;
		}
	}
	
	
	return 0;
}

int network_register_fdevents(server *srv) {
	size_t i;
	
	fdevent_reset(srv->ev);
	
	/* register fdevents after reset */
	for (i = 0; i < srv->srv_sockets.used; i++) {
		server_socket *srv_socket = srv->srv_sockets.ptr[i];
		
		fdevent_register(srv->ev, srv_socket->fd, network_server_handle_fdevent, srv_socket);
		fdevent_event_add(srv->ev, srv_socket->fd, FDEVENT_IN);
	}
	return 0;
}

