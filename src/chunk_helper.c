/**
 * helpers for the network chunk-API
 *
 *
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <fcntl.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include "chunk_helper.h"

#include "sys-mmap.h"
#include "sys-files.h"

#include "log.h"

#include "stat_cache.h"

#define KByte * 1024
#define MByte * 1024 KByte
#define GByte * 1024 MByte

// align mmap offset to 512 kb ( 2 ^ 19 )
#define MMAP_ALIGN (1 << 19)

/* mmap a part of the chunk:
 *  - 'we_want' := min(we_have, 'we_want')
 *  - Tries to map at least max(2 MByte, 'we_want')
 *  - If LOCAL_BUFFERING is enabled, 'we_want' bytes are copied into 
 *    the mem buffer.
 *
 *  - The mem buffer may be smaller than the mmap-length, but the mem buffer
 *    starts at file.mmap.offset
 *  - file.mmap.offset maybe less than (file.start + offset)
 */
LI_API gboolean chunk_mmap(server *srv, connection *con, chunk *c, off_t we_want_max, char **data_start, off_t *data_len) {
	off_t abs_offset, rel_mmap_offset, mmap_offset;
	off_t we_have;
	stat_cache_entry *sce = NULL;
	off_t we_want_to_mmap = 2 * 1024 * 1024;
//	size_t we_want_to_mmap = 2 MByte;

	if (data_start) *data_start = 0;
	if (data_len) *data_len = 0;

	if (HANDLER_ERROR == stat_cache_get_entry(srv, con, c->file.name, &sce)) {
		ERROR("stat_cache_get_entry('%s') failed: %s",
				SAFE_BUF_STR(c->file.name), strerror(errno));
		return FALSE;
	}

	abs_offset = c->file.start + c->offset;
	we_have = c->file.length - c->offset;

	// TODO: align abs_offset and add difference to we_want/we_have

	// check file size
	if (c->file.length + c->file.start > sce->st.st_size) {
		ERROR("file '%s' was shrinked: was %jd, is %jd (%jd, %jd)", 
				SAFE_BUF_STR(c->file.name), (intmax_t) c->file.length + c->file.start, (intmax_t) sce->st.st_size,
				(intmax_t) c->file.start, (intmax_t) c->offset);
		
		return FALSE;
	}

	// mmap at least what we want
	if (we_want_max > we_want_to_mmap) we_want_to_mmap = we_want_max;
	// do not mmap more than we have
	if (we_want_to_mmap > we_have) we_want_to_mmap  = we_have;
	// do not want require than we have
	if (we_want_max > we_have) we_want_max = we_have;

	// Align mmap offset
	rel_mmap_offset = abs_offset & (MMAP_ALIGN - 1);
	mmap_offset = abs_offset & ~(MMAP_ALIGN - 1);
	we_want_to_mmap += rel_mmap_offset;

	if (c->file.mmap.start == MAP_FAILED ||
	    (mmap_offset != c->file.mmap.offset) ||
	    (we_want_to_mmap > (off_t) c->file.mmap.length)) {

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

		if (c->file.mmap.start != MAP_FAILED) {
			munmap(c->file.mmap.start, c->file.mmap.length);
			c->file.mmap.start = MAP_FAILED;
		}

		c->file.mmap.offset = mmap_offset;

		if (-1 == c->file.fd) {  /* open the file if not already open */
			if (-1 == (c->file.fd = open(c->file.name->ptr, O_RDONLY))) {
				ERROR("open failed for '%s': %s", SAFE_BUF_STR(c->file.name), strerror(errno));
				return FALSE;
			}
#ifdef FD_CLOEXEC
			fcntl(c->file.fd, F_SETFD, FD_CLOEXEC);
#endif
		}

		if (MAP_FAILED == (c->file.mmap.start = mmap(0, we_want_to_mmap, PROT_READ, MAP_SHARED, c->file.fd, c->file.mmap.offset))) {
			int mmap_errno = errno;
			off_t r;

			// Try read() fallback
			buffer_prepare_copy(c->mem, we_want_max+1);
			lseek(c->file.fd, abs_offset, SEEK_SET);
			if (-1 == (r = read(c->file.fd, c->mem->ptr, we_want_max))) {
				ERROR("mmap failed for '%s' (fd = %i); fallback read failed too: (mmap error) %s",
					SAFE_BUF_STR(c->file.name), c->file.fd, strerror(mmap_errno));
				close(c->file.fd);

				return FALSE;
			}
			c->mem->used = r;
			c->mem->ptr[r] = '\0';

			if (data_start) *data_start = c->mem->ptr;
			if (data_len) *data_len = r;
			return TRUE;
		}

		c->file.mmap.length = we_want_to_mmap;

#ifdef LOCAL_BUFFERING

		buffer_copy_string_len(c->mem, c->file.mmap.start + rel_mmap_offset, we_want_max);
		if (data_start) *data_start = c->mem->ptr;

#else

#ifdef HAVE_MADVISE
		/* don't advise files < 64Kb */
		if (c->file.mmap.length > (64 KByte) && 
		    0 != madvise(c->file.mmap.start, c->file.mmap.length, MADV_WILLNEED)) {
			ERROR("madvise failed for '%s' (%i): %s", SAFE_BUF_STR(c->file.name), c->file.fd, strerror(errno));
		}
#endif
		if (data_start) *data_start = c->file.mmap.start + rel_mmap_offset;

#endif

		if (data_len) *data_len = we_want_max;
	} else {
#ifdef LOCAL_BUFFERING
		if (c->mem->used - 1 != we_want_max) {
			buffer_copy_string_len(c->mem, c->file.mmap.start + rel_mmap_offset, we_want_max);
		}
		if (data_start) *data_start = c->mem->ptr;
#else
		if (data_start) *data_start = c->file.mmap.start + rel_mmap_offset;
#endif
		if (data_len) *data_len = we_want_max;
	}

	/* chunk_reset() or chunk_free() will cleanup for us */
	return TRUE;
}

/* chunk_get_data:
 *   returns a pointer and the length of a buffer; if given the same we_want_max without changing c->offset
 *   it returns the same pointer (and length).
 *   the return length is not more than we_want_max, but might be less
 */

LI_API gboolean chunk_get_data(server *srv, connection *con, chunk *c, off_t we_want_max, char **data_start, off_t *data_len) {
	off_t len = chunk_length(c);

	if (!len) return FALSE;

	switch (c->type) {
		case MEM_CHUNK:
			if (data_start) *data_start = c->mem->ptr + c->offset;
			if (data_len) *data_len = (len < we_want_max) ? len : we_want_max;
			return TRUE;
		case FILE_CHUNK:
			return chunk_mmap(srv, con, c, we_want_max, data_start, data_len);
		case UNUSED_CHUNK:
			return FALSE;
	}
	return FALSE;
}
