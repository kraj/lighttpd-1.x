#include "network_backends.h"

#ifdef USE_LINUX_SENDFILE

#include "network.h"
#include "log.h"

#include <sys/sendfile.h>

#include <errno.h>
#include <string.h>

int network_write_file_chunk_sendfile(server *srv, connection *con, int fd, chunkqueue *cq, off_t *p_max_bytes) {
	chunk *c;
	ssize_t r;
	off_t offset;
	off_t toSend;
	int cfd;

	if (-1 == (cfd = chunkqueue_open_file(srv, con, cq))) return -1;

	c = cq->first;
	offset = c->file.start + c->offset;
	toSend = c->file.length - c->offset;
	if (toSend > *p_max_bytes) toSend = *p_max_bytes;

	if (0 == toSend) {
		chunkqueue_remove_finished_chunks(cq);
		return 0;
	}

	if (-1 == (r = sendfile(fd, cfd, &offset, toSend))) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			break;
		case EPIPE:
		case ECONNRESET:
			return -2;
		default:
			log_error_write(srv, __FILE__, __LINE__, "ssd",
					"sendfile failed:", strerror(errno), fd);
			return -1;
		}
	}

	if (r >= 0) {
		chunkqueue_mark_written(cq, r);
		*p_max_bytes -= r;
	}

	return (r > 0 && r == toSend) ? 0 : -3;
}

#endif
