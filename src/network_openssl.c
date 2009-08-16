#include "network_backends.h"

#ifdef USE_OPENSSL
#include <sys/types.h>
#include "sys-socket.h"
#include <sys/stat.h>
#include <sys/time.h>
#ifndef _WIN32
#include <sys/resource.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <netdb.h>
#else
#include <sys/types.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "stat_cache.h"
#include "chunk_helper.h"

# include <openssl/ssl.h>
# include <openssl/err.h>

NETWORK_BACKEND_READ(openssl) {
	buffer *b;
	off_t len;
	int read_something = 0;
	off_t max_read = 256 * 1024;
	off_t start_bytes_in = cq->bytes_in;
	network_status_t res;

	UNUSED(srv);
	UNUSED(con);

	do {
		int oerrno;
		b = buffer_init();
		buffer_prepare_copy(b, 8192 + 12); /* ssl-chunk-size is 8kb */
		ERR_clear_error();
		len = SSL_read(sock->ssl, b->ptr, b->size - 1);

		/**
		 * man SSL_read:
		 *
		 * >0   is success
		 * 0    is connection close
		 * <0   is error 
		 */
		if (len <= 0) {
			int r, ssl_err;

			oerrno = errno; /* store the errno for SSL_ERROR_SYSCALL */
			buffer_free(b);

			switch ((r = SSL_get_error(sock->ssl, len))) {
			case SSL_ERROR_WANT_READ:
				return read_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case SSL_ERROR_SYSCALL:
				/**
				 * man SSL_get_error()
				 *
				 * SSL_ERROR_SYSCALL
				 *   Some I/O error occurred.  The OpenSSL error queue may contain more
				 *   information on the error.  If the error queue is empty (i.e.
				 *   ERR_get_error() returns 0), ret can be used to find out more about
				 *   the error: If ret == 0, an EOF was observed that violates the
				 *   protocol.  If ret == -1, the underlying BIO reported an I/O error
				 *   (for socket I/O on Unix systems, consult errno for details).
				 *
				 */
				while((ssl_err = ERR_get_error())) {
					/* get all errors from the error-queue */
					ERROR("ssl-errors: %s", ERR_error_string(ssl_err, NULL));
				}

				if (len == 0) {
					return NETWORK_STATUS_CONNECTION_CLOSE;
				} else {
					switch(oerrno) {
					case EPIPE:
					case ECONNRESET:
						return NETWORK_STATUS_CONNECTION_CLOSE;
					default:
						ERROR("last-errno: (%d) %s", oerrno, strerror(oerrno));
						break;
					}
				}

				return NETWORK_STATUS_FATAL_ERROR;
			case SSL_ERROR_ZERO_RETURN:
				if (len == 0) {
					/* clean shutdown on the remote side */
					return NETWORK_STATUS_CONNECTION_CLOSE;
				}
				/* fall through otherwise */
			default:
				res = NETWORK_STATUS_CONNECTION_CLOSE;
				while((ssl_err = ERR_get_error())) {
					switch (ERR_GET_REASON(ssl_err)) {
					case SSL_R_SSL_HANDSHAKE_FAILURE:
					case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
					case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
					case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
						if (!con->conf.log_ssl_noise) continue;
						break;
					default:
						res = NETWORK_STATUS_FATAL_ERROR;
						break;
					}
					/* get all errors from the error-queue */
					ERROR("ssl-errors: %s", ERR_error_string(ssl_err, NULL));
				}

				return res;
			}
		} else {
			b->used = len;
			b->ptr[b->used++] = '\0';

			read_something = 1;
			chunkqueue_append_buffer(cq, b);
			cq->bytes_in += len;
		}

		if (cq->bytes_in - start_bytes_in > max_read) return NETWORK_STATUS_SUCCESS;
	} while (1);

	return NETWORK_STATUS_FATAL_ERROR;
}


NETWORK_BACKEND_WRITE(openssl) {
	int ssl_r;
	ssize_t r;
	chunk *c;

#define LOCAL_SEND_BUFSIZE (64 * 1024)

	/* the remote side closed the connection before without shutdown request
	 * - IE
	 * - wget
	 * if keep-alive is disabled */

	if (con->keep_alive == 0) {
		SSL_set_shutdown(sock->ssl, SSL_RECEIVED_SHUTDOWN);
	}

	for(c = cq->first; c; c = c->next) {
		char *offset = NULL;
		off_t toSend = 0;

		if (chunk_is_done(c)) continue;

		if (c->type == FILE_CHUNK) {
			toSend = LOCAL_SEND_BUFSIZE;
		} else {
			toSend = chunk_length(c);
		}

		if (!chunk_get_data(srv, con, c, toSend, &offset, &toSend)) {
			ERROR("%s", "Couldn't get data for SSL_write");
			return NETWORK_STATUS_FATAL_ERROR;
		}

		if (toSend <= 0) { // Should never happen
			ERROR("%s", "Got no data from chunk");
			return NETWORK_STATUS_FATAL_ERROR;
		}

		/**
		 * SSL_write man-page
		 *
		 * WARNING
		 *        When an SSL_write() operation has to be repeated because of
		 *        SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE, it must be
		 *        repeated with the same arguments.
		 *
		 * SSL_write(..., 0) return 0 which is handle as an error (Success)
		 * checking toSend and not calling SSL_write() is simpler
		 */

		ERR_clear_error();
		if ((r = SSL_write(sock->ssl, offset, toSend)) <= 0) {
			unsigned long err;

			switch ((ssl_r = SSL_get_error(sock->ssl, r))) {
			case SSL_ERROR_WANT_WRITE:
				break;
			case SSL_ERROR_SYSCALL:
				/* perhaps we have error waiting in our error-queue */
				if (0 != (err = ERR_get_error())) {
					do {
						ERROR("SSL_write(): SSL_get_error() = %d,  SSL_write() = %zd, msg = %s",
								ssl_r, r,
								ERR_error_string(err, NULL));
					} while((err = ERR_get_error()));
				} else if (r == -1) {
					/* no, but we have errno */
					switch(errno) {
					case EPIPE:
					case ECONNRESET:
						return NETWORK_STATUS_CONNECTION_CLOSE;
					default:
						ERROR("SSL_write(): SSL_get_error() = %d,  SSL_write() = %zd, errmsg = %s (%d)",
								ssl_r, r,
								strerror(errno), errno);
						break;
					}
				} else {
					/* neither error-queue nor errno ? */
					ERROR("SSL_write(): SSL_get_error() = %d,  SSL_write() = %zd, errmsg = %s (%d)",
								ssl_r, r,
								strerror(errno), errno);
				}

				return  NETWORK_STATUS_FATAL_ERROR;
			case SSL_ERROR_ZERO_RETURN:
				/* clean shutdown on the remote side */

				if (r == 0) return NETWORK_STATUS_CONNECTION_CLOSE;

				/* fall through */
			default:
				while((err = ERR_get_error())) {
					ERROR("SSL_write(): SSL_get_error() = %d,  SSL_write() = %zd, msg = %s",
							ssl_r, r,
							ERR_error_string(err, NULL));
				}

				return  NETWORK_STATUS_FATAL_ERROR;
			}
		} else {
			c->offset += r;
			cq->bytes_out += r;
		}

		if (!chunk_is_done(c)) {
			/* not finished yet */
			return NETWORK_STATUS_WAIT_FOR_EVENT;
		}
	}

	return NETWORK_STATUS_SUCCESS;
}
#endif

#if 0
network_openssl_init(void) {
	p->write_ssl = network_openssl_write_chunkset;
}
#endif
