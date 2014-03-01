#include "network_backends.h"

#ifdef USE_OPENSSL

#include "network.h"
#include "log.h"

#include <unistd.h>
#include <stdlib.h>

#include <errno.h>
#include <string.h>

# include <openssl/ssl.h>
# include <openssl/err.h>

static int load_next_chunk(server *srv, connection *con, chunkqueue *cq, off_t max_bytes, const char **data, size_t *data_len) {
	chunk * const c = cq->first;

	/* this is a 64k sendbuffer
	 *
	 * it has to stay at the same location all the time to satisfy the needs
	 * of SSL_write to pass the SAME parameter in case of a _WANT_WRITE
	 *
	 * the buffer is allocated once, is NOT realloced and is NOT freed at shutdown
	 * -> we expect a 64k block to 'leak' in valgrind
	 * */
#define LOCAL_SEND_BUFSIZE (64 * 1024)
	static char *local_send_buffer = NULL;

	force_assert(NULL != cq->first);

	switch (c->type) {
	case MEM_CHUNK:
		{
			size_t have = buffer_string_length(c->mem) - c->offset;
			if ((off_t) have > max_bytes) have = max_bytes;

			*data = c->mem->ptr + c->offset;
			*data_len = have;
		}
		return 0;

	case FILE_CHUNK:
		if (NULL == local_send_buffer) {
			local_send_buffer = malloc(LOCAL_SEND_BUFSIZE);
			force_assert(NULL != local_send_buffer);
		}


		{
			off_t offset = c->file.start + c->offset;
			off_t toSend = c->file.length - c->offset;
			int cfd;

			if (-1 == (cfd = chunkqueue_open_file(srv, con, cq))) return -1;
			if (toSend > LOCAL_SEND_BUFSIZE) toSend = LOCAL_SEND_BUFSIZE;
			if (toSend > max_bytes) toSend = max_bytes;

			if (-1 == lseek(cfd, offset, SEEK_SET)) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "lseek: ", strerror(errno));
				return -1;
			}
			if (-1 == (toSend = read(cfd, local_send_buffer, toSend))) {
				log_error_write(srv, __FILE__, __LINE__, "ss", "read: ", strerror(errno));
				return -1;
			}

			*data = local_send_buffer;
			*data_len = toSend;
		}
		return 0;
	}

	return -1;
}


int network_write_chunkqueue_openssl(server *srv, connection *con, SSL *ssl, chunkqueue *cq, off_t max_bytes) {
	/* the remote side closed the connection before without shutdown request
	 * - IE
	 * - wget
	 * if keep-alive is disabled */

	if (con->keep_alive == 0) {
		SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
	}

	chunkqueue_remove_finished_chunks(cq);

	while (max_bytes > 0 && NULL != cq->first) {
		const char *data;
		size_t data_len;
		int r;

		if (0 != load_next_chunk(srv, con, cq, max_bytes, &data, &data_len)) return -1;

		/**
		 * SSL_write man-page
		 *
		 * WARNING
		 *        When an SSL_write() operation has to be repeated because of
		 *        SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE, it must be
		 *        repeated with the same arguments.
		 */

		ERR_clear_error();
		r = SSL_write(ssl, data, data_len);

		if (con->renegotiations > 1 && con->conf.ssl_disable_client_renegotiation) {
			log_error_write(srv, __FILE__, __LINE__, "s", "SSL: renegotiation initiated by client, killing connection");
			return -1;
		}

		if (r <= 0) {
			int ssl_r;
			unsigned long err;

			switch ((ssl_r = SSL_get_error(ssl, r))) {
			case SSL_ERROR_WANT_WRITE:
				return 0; /* try again later */
			case SSL_ERROR_SYSCALL:
				/* perhaps we have error waiting in our error-queue */
				if (0 != (err = ERR_get_error())) {
					do {
						log_error_write(srv, __FILE__, __LINE__, "sdds", "SSL:",
								ssl_r, r,
								ERR_error_string(err, NULL));
					} while((err = ERR_get_error()));
				} else if (r == -1) {
					/* no, but we have errno */
					switch(errno) {
					case EPIPE:
					case ECONNRESET:
						return -2;
					default:
						log_error_write(srv, __FILE__, __LINE__, "sddds", "SSL:",
								ssl_r, r, errno,
								strerror(errno));
						break;
					}
				} else {
					/* neither error-queue nor errno ? */
					log_error_write(srv, __FILE__, __LINE__, "sddds", "SSL (error):",
							ssl_r, r, errno,
							strerror(errno));
				}
				break;

			case SSL_ERROR_ZERO_RETURN:
				/* clean shutdown on the remote side */

				if (r == 0) return -2;

				/* fall through */
			default:
				while((err = ERR_get_error())) {
					log_error_write(srv, __FILE__, __LINE__, "sdds", "SSL:",
							ssl_r, r,
							ERR_error_string(err, NULL));
				}
				break;
			}
			return -1;
		}

		chunkqueue_mark_written(cq, r);
		max_bytes -= r;

		if ((size_t) r < data_len) break; /* try again later */
	}

	return 0;
}
#endif
