#include "network_backends.h"

#include "network.h"
#include "log.h"

#include "sys-socket.h"

#include <unistd.h>

#include <errno.h>
#include <string.h>

#if 0
/* read mmap()ed data into local buffer */
#define LOCAL_BUFFERING 1
#endif

int network_write_mem_chunk(server *srv, connection *con, int fd, chunkqueue *cq, off_t *p_max_bytes) {
	chunk *c;
	off_t c_len;
	ssize_t r;
	UNUSED(con);

	force_assert(NULL != cq->first);
	force_assert(MEM_CHUNK == cq->first->type);

	c = cq->first;
	c_len = buffer_string_length(c->mem) - c->offset;
	if (0 == c_len) {
		chunkqueue_remove_finished_chunks(cq);
		return 0;
	}

	if (c_len > *p_max_bytes) c_len = *p_max_bytes;

#ifdef __WIN32
	if ((r = send(fd, offset, toSend, 0)) < 0) {
		/* no error handling for windows... */
		log_error_write(srv, __FILE__, __LINE__, "ssd", "send failed: ", strerror(errno), fd);

		return -1;
	}
#else
	if ((r = write(fd, c->mem->ptr + c->offset, c_len)) < 0) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			break;
		case EPIPE:
		case ECONNRESET:
			return -2;
		default:
			log_error_write(srv, __FILE__, __LINE__, "ssd",
				"write failed:", strerror(errno), fd);
			return -1;
		}
	}
#endif

	if (r >= 0) {
		*p_max_bytes -= r;
		chunkqueue_mark_written(cq, r);
	}

	return (r > 0 && r == c_len) ? 0 : -3;
}

int network_write_chunkqueue_write(server *srv, connection *con, int fd, chunkqueue *cq, off_t max_bytes) {
	while (max_bytes > 0 && NULL != cq->first) {
		int r = -1;

		switch (cq->first->type) {
		case MEM_CHUNK:
			r = network_write_mem_chunk(srv, con, fd, cq, &max_bytes);
			break;
		case FILE_CHUNK:
			r = network_write_file_chunk_mmap(srv, con, fd, cq, &max_bytes);
			break;
		}

		if (-3 == r) return 0;
		if (0 != r) return r;
	}

	return 0;
}

int network_write_chunkqueue_sendfile(server *srv, connection *con, int fd, chunkqueue *cq, off_t max_bytes) {
	while (max_bytes > 0 && NULL != cq->first) {
		int r = -1;

		switch (cq->first->type) {
		case MEM_CHUNK:
			r = network_writev_mem_chunks(srv, con, fd, cq, &max_bytes);
			break;
		case FILE_CHUNK:
			r = network_write_file_chunk_sendfile(srv, con, fd, cq, &max_bytes);
			break;
		}

		if (-3 == r) return 0;
		if (0 != r) return r;
	}

	return 0;
}
