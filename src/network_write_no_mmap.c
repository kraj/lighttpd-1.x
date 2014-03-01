#include "network_backends.h"

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "stat_cache.h"

#include "sys-socket.h"

#include <unistd.h>

#include <errno.h>
#include <string.h>

int network_write_file_chunk_no_mmap(server *srv, connection *con, int fd, chunkqueue *cq, off_t *p_max_bytes) {
	chunk *c;
	off_t offset, toSend;
	ssize_t r;
	int cfd;

	force_assert(NULL != cq->first);
	force_assert(FILE_CHUNK == cq->first->type);

	c = cq->first;
	offset = c->file.start + c->offset;
	toSend = c->file.length - c->offset;

	if (0 == toSend) {
		chunkqueue_remove_finished_chunks(cq);
		return 0;
	}

	if (-1 == (cfd = chunkqueue_open_file(srv, con, cq))) return -1;

	if (toSend > 64*1024) toSend = 64*1024; /* max read 64kb in one step */
	if (toSend > *p_max_bytes) toSend = *p_max_bytes;

	buffer_string_prepare_copy(srv->tmp_buf, toSend);

	if (-1 == lseek(cfd, offset, SEEK_SET)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "lseek: ", strerror(errno));
		return -1;
	}
	if (-1 == (toSend = read(cfd, srv->tmp_buf->ptr, toSend))) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "read: ", strerror(errno));
		return -1;
	}

#ifdef __WIN32
	if ((r = send(fd, srv->tmp_buf->ptr, toSend, 0)) < 0) {
		/* no error handling for windows... */
		log_error_write(srv, __FILE__, __LINE__, "ssd", "send failed: ", strerror(errno), fd);
		return -1;
	}
#else /* __WIN32 */
	if ((r = write(fd, srv->tmp_buf->ptr, toSend)) < 0) {
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
#endif /* __WIN32 */

	if (r >= 0) {
		*p_max_bytes -= r;
		chunkqueue_mark_written(cq, r);
	}

	return (r > 0 && r == toSend) ? 0 : -3;
}
