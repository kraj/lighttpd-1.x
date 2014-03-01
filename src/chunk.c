/**
 * the network chunk-API
 *
 *
 */

#include "chunk.h"
#include "base.h"
#include "log.h"
#include "file.h"

#include <sys/types.h>
#include <sys/stat.h>
#include "sys-mmap.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>

static chunkfile *chunkfile_init(void) {
	chunkfile *cf = calloc(1, sizeof(*cf));
	force_assert(NULL != cf);

	cf->refcount = 1;
	cf->fd = -1;
	cf->name = NULL;
	cf->is_temp = 0;

	return cf;
}

static void chunkfile_free(chunkfile *cf) {
	force_assert(NULL != cf && 0 == cf->refcount);
	if (-1 != cf->fd) close(cf->fd);
	if (NULL != cf->name) {
		if (cf->is_temp && !buffer_string_is_empty(cf->name)) {
			unlink(cf->name->ptr);
		}
		buffer_free(cf->name);
	}
	free(cf);
}

chunkfile *chunkfile_new(int fd) {
	chunkfile *cf = chunkfile_init();
	cf->fd = fd;
	return cf;
}

static chunkfile *chunkfile_named_new(buffer *name) {
	chunkfile *cf = chunkfile_init();
	cf->name = buffer_init_buffer(name);
	return cf;
}

static chunkfile *chunkfile_tmp_named_new(int fd, buffer *name) {
	chunkfile *cf = chunkfile_init();
	cf->fd = fd;
	cf->name = buffer_init_buffer(name);
	cf->is_temp = 1;
	return cf;
}


void chunkfile_acquire(chunkfile *cf) {
	force_assert(NULL != cf && cf->refcount > 0);

	force_assert(cf->refcount + 1 > cf->refcount);
	++cf->refcount;
}
void chunkfile_release(chunkfile **pcf) {
	chunkfile *cf;
	if (NULL == pcf) return;
	cf = *pcf;
	if (NULL == cf) return;
	*pcf = NULL;
	force_assert(cf->refcount > 0);
	if (0 == --cf->refcount) {
		chunkfile_free(cf);
	}
}

chunkqueue *chunkqueue_init(void) {
	chunkqueue *cq;

	cq = calloc(1, sizeof(*cq));
	force_assert(NULL != cq);

	cq->first = NULL;
	cq->last = NULL;

	cq->unused = NULL;

	return cq;
}

static chunk *chunk_init(void) {
	chunk *c;

	c = calloc(1, sizeof(*c));
	force_assert(NULL != c);

	c->type = MEM_CHUNK;
	c->mem = buffer_init();
	c->file.fileref = NULL;
	c->file.start = c->file.length = c->file.mmap.offset = 0;
	c->file.mmap.start = MAP_FAILED;
	c->file.mmap.length = 0;
	c->offset = 0;
	c->next = NULL;

	return c;
}

static void chunk_reset(chunk *c) {
	if (NULL == c) return;

	c->type = MEM_CHUNK;

	buffer_reset(c->mem);

	chunkfile_release(&c->file.fileref);

	if (MAP_FAILED != c->file.mmap.start) {
		munmap(c->file.mmap.start, c->file.mmap.length);
		c->file.mmap.start = MAP_FAILED;
	}
	c->file.start = c->file.length = c->file.mmap.offset = 0;
	c->file.mmap.length = 0;
	c->offset = 0;
	c->next = NULL;
}

static void chunk_free(chunk *c) {
	if (NULL == c) return;

	chunk_reset(c);

	buffer_free(c->mem);

	free(c);
}

void chunkqueue_free(chunkqueue *cq) {
	chunk *c, *pc;

	if (NULL == cq) return;

	for (c = cq->first; c; ) {
		pc = c;
		c = c->next;
		chunk_free(pc);
	}

	for (c = cq->unused; c; ) {
		pc = c;
		c = c->next;
		chunk_free(pc);
	}

	free(cq);
}

static void chunkqueue_push_unused_chunk(chunkqueue *cq, chunk *c) {
	force_assert(NULL != cq && NULL != c);

	/* keep at max 4 chunks in the 'unused'-cache */
	if (cq->unused_chunks > 4) {
		chunk_free(c);
	} else {
		chunk_reset(c);
		c->next = cq->unused;
		cq->unused = c;
		cq->unused_chunks++;
	}
}

static chunk *chunkqueue_get_unused_chunk(chunkqueue *cq) {
	chunk *c;

	force_assert(NULL != cq);

	/* check if we have a unused chunk */
	if (0 == cq->unused) {
		c = chunk_init();
	} else {
		/* take the first element from the list (a stack) */
		c = cq->unused;
		cq->unused = c->next;
		c->next = NULL;
		cq->unused_chunks--;
	}

	return c;
}

static void chunkqueue_prepend_chunk(chunkqueue *cq, chunk *c) {
	c->next = cq->first;
	cq->first = c;

	if (NULL == cq->last) {
		cq->last = c;
	}
}

static void chunkqueue_append_chunk(chunkqueue *cq, chunk *c) {
	if (cq->last) {
		cq->last->next = c;
	}
	cq->last = c;

	if (NULL == cq->first) {
		cq->first = c;
	}
}

void chunkqueue_reset(chunkqueue *cq) {
	chunk *cur = cq->first;

	cq->first = cq->last = NULL;

	while (NULL != cur) {
		chunk *next = cur->next;
		chunkqueue_push_unused_chunk(cq, cur);
		cur = next;
	}

	cq->bytes_in = 0;
	cq->bytes_out = 0;
}

void chunkqueue_append_file(chunkqueue *cq, buffer *fn, off_t offset, off_t len) {
	chunkfile *cf = chunkfile_named_new(fn);
	chunkqueue_append_chunkfile(cq, cf, offset, len);
	chunkfile_release(&cf);
}

void chunkqueue_append_chunkfile(chunkqueue *cq, chunkfile *cf, off_t offset, off_t len) {
	chunk *c;

	if (0 == len) return;

	c = chunkqueue_get_unused_chunk(cq);
	chunkfile_acquire(cf);

	c->type = FILE_CHUNK;
	c->file.fileref = cf;

	c->file.start = offset;
	c->file.length = len;
	c->offset = 0;

	chunkqueue_append_chunk(cq, c);
	cq->bytes_in += len;
}

void chunkqueue_append_buffer(chunkqueue *cq, buffer *mem) {
	chunk *c;

	if (buffer_string_is_empty(mem)) return;

	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	force_assert(NULL != c->mem);
	buffer_move(c->mem, mem);

	chunkqueue_append_chunk(cq, c);
	cq->bytes_in += buffer_string_length(c->mem);
}

void chunkqueue_prepend_buffer(chunkqueue *cq, buffer *mem) {
	chunk *c;

	if (buffer_string_is_empty(mem)) return;

	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	force_assert(NULL != c->mem);
	buffer_move(c->mem, mem);

	chunkqueue_prepend_chunk(cq, c);
	cq->bytes_in += buffer_string_length(c->mem);
}


void chunkqueue_append_mem(chunkqueue *cq, const char * mem, size_t len) {
	chunk *c;

	if (0 == len) return;

	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	buffer_copy_string_len(c->mem, mem, len);

	chunkqueue_append_chunk(cq, c);
	cq->bytes_in += len;
}

void chunkqueue_get_memory(chunkqueue *cq, char **mem, size_t *len, size_t min_size, size_t alloc_size) {
	static const size_t REALLOC_MAX_SIZE = 256;
	chunk *c;
	buffer *b;
	char *dummy_mem;
	size_t dummy_len;

	force_assert(NULL != cq);
	if (NULL == mem) mem = &dummy_mem;
	if (NULL == len) len = &dummy_len;

	/* default values: */
	if (0 == min_size) min_size = 1024;
	if (0 == alloc_size) alloc_size = 4096;
	if (alloc_size < min_size) alloc_size = min_size;

	if (NULL != cq->last && MEM_CHUNK == cq->last->type) {
		size_t have;

		b = cq->last->mem;
		have = buffer_string_space(b);

		/* unused buffer: allocate space */
		if (buffer_string_is_empty(b)) {
			buffer_string_prepare_copy(b, alloc_size);
			have = buffer_string_space(b);
		}
		/* if buffer is really small just make it bigger */
		else if (have < min_size && b->size <= REALLOC_MAX_SIZE) {
			size_t cur_len = buffer_string_length(b);
			size_t new_size = cur_len + min_size, append;
			if (new_size < alloc_size) new_size = alloc_size;

			append = new_size - cur_len;
			if (append >= min_size) {
				buffer_string_prepare_append(b, append);
				have = buffer_string_space(b);
			}
		}

		/* return pointer into existing buffer if large enough */
		if (have >= min_size) {
			*mem = b->ptr + buffer_string_length(b);
			*len = have;
			return;
		}
	}

	/* allocate new chunk */
	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	chunkqueue_append_chunk(cq, c);

	b = c->mem;
	buffer_string_prepare_append(b, alloc_size);

	*mem = b->ptr + buffer_string_length(b);
	*len = buffer_string_space(b);
}

void chunkqueue_use_memory(chunkqueue *cq, size_t len) {
	buffer *b;

	force_assert(NULL != cq);
	force_assert(NULL != cq->last && MEM_CHUNK == cq->last->type);
	b = cq->last->mem;

	if (len > 0) {
		buffer_commit(b, len);
		cq->bytes_in += len;
	} else if (buffer_string_is_empty(b)) {
		/* unused buffer: can't remove chunk easily from
		 * end of list, so just reset the buffer
		 */
		buffer_reset(b);
	}
}

void chunkqueue_set_tempdirs(chunkqueue *cq, array *tempdirs) {
	force_assert(NULL != cq);
	cq->tempdirs = tempdirs;
}

void chunkqueue_steal(chunkqueue *dest, chunkqueue *src, off_t len) {
	while (len > 0) {
		chunk *c = src->first;
		off_t clen = 0, use;

		if (NULL == c) break;

		switch (c->type) {
		case MEM_CHUNK:
			clen = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			clen = c->file.length;
			break;
		}
		force_assert(clen >= c->offset);
		clen -= c->offset;
		use = len >= clen ? clen : len;

		src->bytes_out += use;
		dest->bytes_in += use;
		len -= use;

		if (0 == clen) {
			/* drop empty chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunkqueue_push_unused_chunk(src, c);
			continue;
		}

		if (use == clen) {
			/* move complete chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;

			chunkqueue_append_chunk(dest, c);
			continue;
		}

		/* partial chunk with length "use" */

		switch (c->type) {
		case MEM_CHUNK:
			chunkqueue_append_mem(dest, c->mem->ptr + c->offset, use);
			break;
		case FILE_CHUNK:
			chunkqueue_append_chunkfile(dest, c->file.fileref, c->file.start + c->offset, use);
			break;
		}

		c->offset += use;
		force_assert(0 == len);
	}
}

static chunk *chunkqueue_get_append_tempfile(chunkqueue *cq) {
	chunk *c;
	chunkfile *cf;
	buffer *template = buffer_init_string("/var/tmp/lighttpd-upload-XXXXXX");
	int fd;

	if (cq->tempdirs && cq->tempdirs->used) {
		size_t i;

		/* we have several tempdirs, only if all of them fail we jump out */

		for (i = 0; i < cq->tempdirs->used; i++) {
			data_string *ds = (data_string *)cq->tempdirs->data[i];

			buffer_copy_buffer(template, ds->value);
			buffer_append_slash(template);
			buffer_append_string_len(template, CONST_STR_LEN("lighttpd-upload-XXXXXX"));

			if (-1 != (fd = mkstemp(template->ptr))) break;
		}
	} else {
		fd = mkstemp(template->ptr);
	}

	if (-1 == fd) {
		buffer_free(template);
		return NULL;
	}

	cf = chunkfile_tmp_named_new(fd, template);
	/* append_chunkfile doesn't work as chunk is empty */
	c = chunkqueue_get_unused_chunk(cq);
	c->type = FILE_CHUNK;
	c->file.fileref = cf;
	c->file.length = 0;
	chunkqueue_append_chunk(cq, c);

	buffer_free(template);

	return c;
}

static int chunkqueue_append_to_tempfile(server *srv, chunkqueue *dest, const char *mem, size_t len) {
	chunk *dst_c = NULL;
	ssize_t written;
	/* copy everything to max 1Mb sized tempfiles */

	/*
	 * if the last chunk is
	 * - smaller than 1Mb (size < 1Mb)
	 * - not read yet (offset == 0)
	 * -> append to it
	 * otherwise
	 * -> create a new chunk
	 *
	 * */

	if (NULL != dest->last
		&& FILE_CHUNK != dest->last->type
		&& dest->last->file.fileref->is_temp
		&& -1 != dest->last->file.fileref->fd
		&& 0 == dest->last->offset) {
		/* ok, take the last chunk for our job */
		dst_c = dest->last;

		if (dest->last->file.length >= 1 * 1024 * 1024) {
			/* the chunk is too large now, close it */
			close(dst_c->file.fileref->fd);
			dst_c->file.fileref->fd = -1;
			dst_c = chunkqueue_get_append_tempfile(dest);
		}
	} else {
		dst_c = chunkqueue_get_append_tempfile(dest);
	}

	if (NULL == dst_c) {
		/* we don't have file to write to,
		 * EACCES might be one reason.
		 *
		 * Instead of sending 500 we send 413 and say the request is too large
		 */

		log_error_write(srv, __FILE__, __LINE__, "sbs",
			"denying upload as opening temp-file for upload failed:",
			dst_c->file.fileref->name, strerror(errno));

		return -1;
	}

	if (0 > (written = write(dst_c->file.fileref->fd, mem, len)) || (size_t) written != len) {
		/* write failed for some reason ... disk full ? */
		log_error_write(srv, __FILE__, __LINE__, "sbs",
				"denying upload as writing to file failed:",
				dst_c->file.fileref->name, strerror(errno));

		close(dst_c->file.fileref->fd);
		dst_c->file.fileref->fd = -1;

		return -1;
	}

	dst_c->file.length += len;

	return 0;
}

int chunkqueue_steal_with_tempfiles(server *srv, chunkqueue *dest, chunkqueue *src, off_t len) {
	while (len > 0) {
		chunk *c = src->first;
		off_t clen = 0, use;

		if (NULL == c) break;

		switch (c->type) {
		case MEM_CHUNK:
			clen = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			clen = c->file.length;
			break;
		}
		force_assert(clen >= c->offset);
		clen -= c->offset;
		use = len >= clen ? clen : len;

		src->bytes_out += use;
		dest->bytes_in += use;
		len -= use;

		if (0 == clen) {
			/* drop empty chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunkqueue_push_unused_chunk(src, c);
			continue;
		}

		if (FILE_CHUNK == c->type) {
			if (use == clen) {
				/* move complete chunk */
				src->first = c->next;
				if (c == src->last) src->last = NULL;

				chunkqueue_append_chunk(dest, c);
			} else {
				/* partial chunk with length "use" */
				/* tempfile flag is in "last" chunk after the split */
				chunkqueue_append_chunkfile(dest, c->file.fileref, c->file.start + c->offset, use);

				c->offset += use;
				force_assert(0 == len);
			}
			continue;
		}

		/* store "use" bytes from memory chunk in tempfile */
		if (0 != chunkqueue_append_to_tempfile(srv, dest, c->mem->ptr + c->offset, use)) {
			/* undo counters */
			src->bytes_out -= use;
			dest->bytes_in -= use;
			return -1;
		}


		c->offset += use;
		if (use == clen) {
			/* finished chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunkqueue_push_unused_chunk(src, c);
		}
	}

	return 0;
}

off_t chunkqueue_length(chunkqueue *cq) {
	off_t len = 0;
	chunk *c;

	for (c = cq->first; c; c = c->next) {
		off_t c_len = 0;

		switch (c->type) {
		case MEM_CHUNK:
			c_len = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			c_len = c->file.length;
			break;
		}
		force_assert(c_len >= c->offset);
		len += c_len - c->offset;
	}

	return len;
}

int chunkqueue_is_empty(chunkqueue *cq) {
	return NULL == cq->first;
}

void chunkqueue_mark_written(chunkqueue *cq, off_t len) {
	off_t written = len;
	chunk *c;

	for (c = cq->first; NULL != c; c = cq->first) {
		off_t c_len = 0;

		switch (c->type) {
		case MEM_CHUNK:
			c_len = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			c_len = c->file.length;
			break;
		}
		force_assert(c_len >= c->offset);
		c_len -= c->offset;

		if (0 == written && 0 != c_len) break; /* no more finished chunks */

		if (written >= c_len) { /* chunk got finished */
			c->offset += c_len;
			written -= c_len;

			cq->first = c->next;
			if (c == cq->last) cq->last = NULL;

			chunkqueue_push_unused_chunk(cq, c);
		} else { /* partial chunk */
			c->offset += written;
			written = 0;
			break; /* chunk not finished */
		}
	}

	force_assert(0 == written);
	cq->bytes_out += len;
}

void chunkqueue_remove_finished_chunks(chunkqueue *cq) {
	chunk *c;

	for (c = cq->first; c; c = cq->first) {
		off_t c_len = 0;

		switch (c->type) {
		case MEM_CHUNK:
			c_len = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			c_len = c->file.length;
			break;
		}
		force_assert(c_len >= c->offset);

		if (c_len > c->offset) break; /* not finished yet */

		cq->first = c->next;
		if (c == cq->last) cq->last = NULL;

		chunkqueue_push_unused_chunk(cq, c);
	}
}


int chunkqueue_open_file(server *srv, connection *con, chunkqueue *cq) {
	chunk *c;
	off_t offset, toSend;
	struct stat st;
	int fd;

	force_assert(NULL != cq->first);
	force_assert(FILE_CHUNK == cq->first->type);
	c = cq->first;
	offset = c->file.start + c->offset;
	toSend = c->file.length - c->offset;

	if (-1 == (fd = c->file.fileref->fd)) {
		if (-1 == (c->file.fileref->fd = fd = file_open(srv, con, c->file.fileref->name, &st, 0))) return -1;
	} else if (-1 == fstat(fd, &st)) {
		log_error_write(srv, __FILE__, __LINE__, "sbss", "fstat failed:", c->file.fileref->name, ":", strerror(errno));
		return -1;
	}

	if (offset > st.st_size || toSend > st.st_size || offset > st.st_size - toSend) {
		log_error_write(srv, __FILE__, __LINE__, "sb", "file was shrinked:", c->file.fileref->name);
		return -1;
	}

	return fd;
}

int chunkqueue_open_trusted_file(server *srv, chunkqueue *cq) {
	chunk *c;
	off_t offset, toSend;
	struct stat st;
	int fd;
	const int flags
		= O_RDONLY
#ifdef O_BINARY
		| O_BINARY
#endif
#ifdef O_NONBLOCK
		| O_NONBLOCK /* don't block on opening named files */
#endif
#ifdef O_NOCTTY
		| O_NOCTTY /* don't allow overtaking controlling terminal */
#endif
	;

	force_assert(NULL != cq->first);
	force_assert(FILE_CHUNK == cq->first->type);
	c = cq->first;
	offset = c->file.start + c->offset;
	toSend = c->file.length - c->offset;

	if (-1 == (fd = c->file.fileref->fd)) {
		if (-1 == (c->file.fileref->fd = fd = open(c->file.fileref->name->ptr, flags))) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "open failed:", c->file.fileref->name, ":", strerror(errno));
			return -1;
		}
		fd_close_on_exec(fd);
	}

	if (-1 == fstat(fd, &st)) {
		log_error_write(srv, __FILE__, __LINE__, "sbss", "fstat failed:", c->file.fileref->name, ":", strerror(errno));
		return -1;
	}

	if (!S_ISREG(st.st_mode)) {
		close(fd);
		c->file.fileref->fd = -1;
		log_error_write(srv, __FILE__, __LINE__, "sb", "not a regular file:", c->file.fileref->name);
		return -1;
	}

	if (offset > st.st_size || toSend > st.st_size || offset > st.st_size - toSend) {
		log_error_write(srv, __FILE__, __LINE__, "sbooo", "file was shrinked:", c->file.fileref->name, offset, toSend, st.st_size);
		return -1;
	}

	return fd;
}
