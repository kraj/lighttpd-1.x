#include "network_backends.h"

#ifdef USE_OPENSSL
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "stat_cache.h"

# include <openssl/ssl.h>
# include <openssl/err.h>

NETWORK_BACKEND_READ_SSL(openssl) {
	server_socket *srv_sock = con->srv_socket;
	int r, ssl_err;
	buffer *b;
	off_t len;

	b = chunkqueue_get_append_buffer(con->read_queue);
	buffer_prepare_copy(b, 4096);
	len = SSL_read(con->ssl, b->ptr, b->size - 1);

	if (len < 0) {
		switch ((r = SSL_get_error(con->ssl, len))) {
		case SSL_ERROR_WANT_READ:
			return NETWORK_STATUS_WAIT_FOR_EVENT;
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
				log_error_write(srv, __FILE__, __LINE__, "sds", "SSL:",
						r, ERR_error_string(ssl_err, NULL));
			}

			switch(errno) {
			default:
				log_error_write(srv, __FILE__, __LINE__, "sddds", "SSL:",
						len, r, errno,
						strerror(errno));
				break;
			}

			break;
		case SSL_ERROR_ZERO_RETURN:
			/* clean shutdown on the remote side */

			if (r == 0) {
				/* FIXME: later */
			}

			/* fall thourgh */
		default:
			while((ssl_err = ERR_get_error())) {
				/* get all errors from the error-queue */
				log_error_write(srv, __FILE__, __LINE__, "sds", "SSL:",
						r, ERR_error_string(ssl_err, NULL));
			}
			break;
		}
	}


	return NETWORK_STATUS_SUCCESS;
}


NETWORK_BACKEND_WRITE_SSL(openssl) {
	int ssl_r;
	chunk *c;
	size_t chunks_written = 0;

	/* this is a 64k sendbuffer
	 *
	 * it has to stay at the same location all the time to satisfy the needs
	 * of SSL_write to pass the SAME parameter in case of a _WANT_WRITE
	 *
	 * the buffer is allocated once, is NOT realloced and is NOT freed at shutdown
	 * -> we expect a 64k block to 'leak' in valgrind
	 *
	 *
	 * In reality we would like to use mmap() but we don't have a guarantee that
	 * we get the same mmap() address for each call. On openbsd the mmap() address
	 * even randomized.
	 *   That means either we keep the mmap() open or we do a read() into a
	 * constant buffer
	 * */
#define LOCAL_SEND_BUFSIZE (64 * 1024)
	static char *local_send_buffer = NULL;

	/* the remote side closed the connection before without shutdown request
	 * - IE
	 * - wget
	 * if keep-alive is disabled */

	if (con->keep_alive == 0) {
		SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
	}

	for(c = cq->first; c; c = c->next) {
		int chunk_finished = 0;

		switch(c->type) {
		case MEM_CHUNK: {
			char * offset;
			size_t toSend;
			ssize_t r = 0;

			if (c->mem->used == 0) {
				chunk_finished = 1;
				break;
			}

			offset = c->mem->ptr + c->offset;
			toSend = c->mem->used - 1 - c->offset;

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

			if (toSend != 0 && (r = SSL_write(ssl, offset, toSend)) <= 0) {
				unsigned long err;

				switch ((ssl_r = SSL_get_error(ssl, r))) {
				case SSL_ERROR_WANT_WRITE:
					break;
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

					return  -1;
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

					return  -1;
				}
			} else {
				c->offset += r;
				cq->bytes_out += r;
			}

			if (c->offset == (off_t)c->mem->used - 1) {
				chunk_finished = 1;
			}

			break;
		}
		case FILE_CHUNK: {
			char *s;
			ssize_t r;
			stat_cache_entry *sce = NULL;
			int ifd;
			int write_wait = 0;

			if (HANDLER_ERROR == stat_cache_get_entry(srv, con, c->file.name, &sce)) {
				log_error_write(srv, __FILE__, __LINE__, "sb",
						strerror(errno), c->file.name);
				return -1;
			}

			if (NULL == local_send_buffer) {
				local_send_buffer = malloc(LOCAL_SEND_BUFSIZE);
				assert(local_send_buffer);
			}

			do {
				off_t offset = c->file.start + c->offset;
				off_t toSend = c->file.length - c->offset;

				if (toSend > LOCAL_SEND_BUFSIZE) toSend = LOCAL_SEND_BUFSIZE;

				if (-1 == (ifd = open(c->file.name->ptr, O_RDONLY))) {
					log_error_write(srv, __FILE__, __LINE__, "ss", "open failed:", strerror(errno));

					return -1;
				}


				lseek(ifd, offset, SEEK_SET);
				if (-1 == (toSend = read(ifd, local_send_buffer, toSend))) {
					close(ifd);
					log_error_write(srv, __FILE__, __LINE__, "ss", "read failed:", strerror(errno));
					return -1;
				}

				s = local_send_buffer;

				close(ifd);

				if ((r = SSL_write(ssl, s, toSend)) <= 0) {
					unsigned long err;

					switch ((ssl_r = SSL_get_error(ssl, r))) {
					case SSL_ERROR_WANT_WRITE:
						write_wait = 1;
						break;
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

						return  -1;
					case SSL_ERROR_ZERO_RETURN:
						/* clean shutdown on the remote side */

						if (r == 0)  return -2;

						/* fall thourgh */
					default:
						while((err = ERR_get_error())) {
							log_error_write(srv, __FILE__, __LINE__, "sdds", "SSL:",
									ssl_r, r,
									ERR_error_string(err, NULL));
						}

						return -1;
					}
				} else {
					c->offset += r;
					cq->bytes_out += r;
				}

				if (c->offset == c->file.length) {
					chunk_finished = 1;
				}
			} while(!chunk_finished && !write_wait);

			break;
		}
		default:
			log_error_write(srv, __FILE__, __LINE__, "s", "type not known");

			return -1;
		}

		if (!chunk_finished) {
			/* not finished yet */

			break;
		}

		chunks_written++;
	}

	return chunks_written;
}
#endif

#if 0
network_openssl_init(void) {
	p->write_ssl = network_openssl_write_chunkset;
}
#endif
