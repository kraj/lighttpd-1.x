#include "network_backends.h"

#ifdef USE_MMAP

#include "network.h"
#include "log.h"
#include "sys-mmap.h"

#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

static volatile int sigbus_jmp_valid;
static sigjmp_buf sigbus_jmp;

static void sigbus_handler(int sig) {
	UNUSED(sig);
	if (sigbus_jmp_valid) siglongjmp(sigbus_jmp, 1);
	log_failed_assert(__FILE__, __LINE__, "SIGBUS");
}


#if 0
/* read mmap()ed data into local buffer */
#define LOCAL_BUFFERING 1
#endif

int network_write_file_chunk_mmap(server *srv, connection *con, int fd, chunkqueue *cq, off_t *p_max_bytes) {
	chunk *c;
	off_t offset, toSend, file_end;
	ssize_t r;
	size_t mmap_offset, mmap_avail;
	const char *data;
	int cfd;

	force_assert(NULL != cq->first);
	force_assert(FILE_CHUNK == cq->first->type);

	c = cq->first;
	offset = c->file.start + c->offset; /* file offset */
	toSend = c->file.length - c->offset;
	file_end = c->file.start + c->file.length; /* offset to file end in this chunk */

	if (0 == toSend) {
		chunkqueue_remove_finished_chunks(cq);
		return 0;
	}

	if (-1 == (cfd = chunkqueue_open_file(srv, con, cq))) return -1;

	/* setup SIGBUS handler, but don't activate sigbus_jmp_valid yet */
	if (0 != sigsetjmp(sigbus_jmp, 1)) {
		sigbus_jmp_valid = 0;

		log_error_write(srv, __FILE__, __LINE__, "sbd", "SIGBUS in mmap:",
			c->file.name, cfd);

		munmap(c->file.mmap.start, c->file.mmap.length);
		c->file.mmap.start = MAP_FAILED;
#ifdef LOCAL_BUFFERING
		buffer_reset(c->mem);
#endif

		return -1;
	}

	signal(SIGBUS, sigbus_handler);

	/* mmap the buffer if offset is outside old mmap area or not mapped at all */
	if (MAP_FAILED == c->file.mmap.start
		|| offset < c->file.mmap.offset
		|| offset >= (off_t)(c->file.mmap.offset + c->file.mmap.length)) {

		if (MAP_FAILED != c->file.mmap.start) {
			munmap(c->file.mmap.start, c->file.mmap.length);
			c->file.mmap.start = MAP_FAILED;
		}

		/* Optimizations for the future:
		 *
		 * adaptive mem-mapping
		 *   the problem:
		 *     we mmap() the whole file. If someone has alot large files and 32bit
		 *     machine the virtual address area will be unrun and we will have a failing
		 *     mmap() call.
		 *   solution:
		 *     only mmap 16M in one chunk and move the window as soon as we have finished
		 *     the first 8M
		 *
		 * read-ahead buffering
		 *   the problem:
		 *     sending out several large files in parallel trashes the read-ahead of the
		 *     kernel leading to long wait-for-seek times.
		 *   solutions: (increasing complexity)
		 *     1. use madvise
		 *     2. use a internal read-ahead buffer in the chunk-structure
		 *     3. use non-blocking IO for file-transfers
		 *   */

		c->file.mmap.offset = offset & ~(4095); /* align at 4kb */

		/* all mmap()ed areas are 512kb except the last which might be smaller */
		c->file.mmap.length = 512*1024;
		if (c->file.mmap.offset > file_end - (off_t)c->file.mmap.length) {
			c->file.mmap.length = file_end - c->file.mmap.offset;
		}

		if (MAP_FAILED == (c->file.mmap.start = mmap(NULL, c->file.mmap.length, PROT_READ, MAP_SHARED, cfd, c->file.mmap.offset))) {
			log_error_write(srv, __FILE__, __LINE__, "ssbd", "mmap failed:",
				strerror(errno), c->file.name, cfd);
			return -1;
		}

#ifdef LOCAL_BUFFERING
		sigbus_jmp_valid = 1;
		buffer_copy_string_len(c->mem, c->file.mmap.start, c->file.mmap.length);
		sigbus_jmp_valid = 0;
#else
#ifdef HAVE_MADVISE
		/* don't advise files < 64Kb */
		if (c->file.mmap.length > (64*1024)) {
			/* darwin 7 is returning EINVAL all the time and I don't know how to
			 * detect this at runtime.
			 *
			 * ignore the return value for now */
			madvise(c->file.mmap.start, c->file.mmap.length, MADV_WILLNEED);
		}
#endif
#endif
	}

	force_assert(offset >= c->file.mmap.offset);
	mmap_offset = offset - c->file.mmap.offset;
	force_assert(c->file.mmap.length > mmap_offset);
	mmap_avail = c->file.mmap.length - mmap_offset;
	if (toSend > (off_t) mmap_avail) toSend = mmap_avail;

#ifdef LOCAL_BUFFERING
	data = c->mem->ptr + mmap_offset;
#else
	data = c->file.mmap.start + mmap_offset;
#endif

	sigbus_jmp_valid = 1;
	r = write(fd, data, toSend);
	sigbus_jmp_valid = 0;

	if (r < 0) {
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

	if (r >= 0) {
		*p_max_bytes -= r;
		chunkqueue_mark_written(cq, r);
	}

	return (r > 0 && r == toSend) ? 0 : -3;
}

#endif
