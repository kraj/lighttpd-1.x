/**
 * the network chunk-API
 * 
 * 
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "chunk.h"

chunkqueue *chunkqueue_init(void) {
	chunkqueue *cq;
	
	cq = calloc(1, sizeof(*cq));
	
	cq->first = NULL;
	cq->last = NULL;
	
	cq->unused = NULL;
	
	return cq;
}

static chunk *chunk_init(void) {
	chunk *c;
	
	c = calloc(1, sizeof(*c));
	
	/* c->mem overlaps with c->data.file.name */
	c->data.mem = buffer_init();
	c->next = NULL;
	
	return c;
}

static void chunk_free(chunk *c) {
	if (!c) return;
	
	/* c->data.mem overlaps with c->data.file.name */
	switch (c->type) {
	case MEM_CHUNK: buffer_free(c->data.mem); break;
	case FILE_CHUNK: break;
	default: break;	
	}
	
	free(c);
}

void chunkqueue_free(chunkqueue *cq) {
	chunk *c, *pc;
	
	if (!cq) return;
	
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

static chunk *chunkqueue_get_unused_chunk(chunkqueue *cq) {
	chunk *c;
	
	/* check if we have a unused chunk */
	if (!cq->unused) {
		c = chunk_init();
	} else {
		/* take the first element from the list (a stack) */
		c = cq->unused;
		cq->unused = c->next;
		c->next = NULL;
	}
	
	return c;
}

static int chunkqueue_prepend_chunk(chunkqueue *cq, chunk *c) {
	c->next = cq->first;
	cq->first = c;
	
	if (cq->last == NULL) {
		cq->last = c;
	}
	
	return 0;
}

static int chunkqueue_append_chunk(chunkqueue *cq, chunk *c) {
	if (cq->last) {
		cq->last->next = c;
	}
	cq->last = c;
	
	if (cq->first == NULL) {
		cq->first = c;
	}
	
	return 0;
}

void chunkqueue_reset(chunkqueue *cq) {
	/* move everything to the unused queue */
	
	if (cq->last == NULL) return;
	
	cq->last->next = cq->unused;
	cq->unused = cq->first;
	
	/* disconnect active chain */
	cq->first = cq->last = NULL;
}

int chunkqueue_append_file(chunkqueue *cq, file_cache_entry *fce, off_t offset, off_t len) {
	chunk *c;
	
	if (len == 0) return 0;
	
	c = chunkqueue_get_unused_chunk(cq);
	
	c->type = FILE_CHUNK;
	
	c->data.file.fce = fce;
	c->data.file.offset = offset;
	c->data.file.length = len;
	c->offset = 0;
	
	chunkqueue_append_chunk(cq, c);
	
	return 0;
}

int chunkqueue_append_buffer(chunkqueue *cq, buffer *mem) {
	chunk *c;
	
	if (mem->used == 0) return 0;
	
	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	c->offset = 0;
	buffer_copy_string_buffer(c->data.mem, mem);
	
	chunkqueue_append_chunk(cq, c);
	
	return 0;
}

int chunkqueue_prepend_buffer(chunkqueue *cq, buffer *mem) {
	chunk *c;
	
	if (mem->used == 0) return 0;
	
	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	c->offset = 0;
	buffer_copy_string_buffer(c->data.mem, mem);
	
	chunkqueue_prepend_chunk(cq, c);
	
	return 0;
}

int chunkqueue_append_mem(chunkqueue *cq, const char * mem, size_t len) {
	chunk *c;
	
	if (len == 0) return 0;
	
	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	c->offset = 0;
	buffer_copy_string_len(c->data.mem, mem, len - 1);
	
	chunkqueue_append_chunk(cq, c);
	
	return 0;
}

buffer * chunkqueue_get_prepend_buffer(chunkqueue *cq) {
	chunk *c;
	
	c = chunkqueue_get_unused_chunk(cq);
	
	c->type = MEM_CHUNK;
	c->offset = 0;
	buffer_reset(c->data.mem);
	
	chunkqueue_prepend_chunk(cq, c);
	
	return c->data.mem;
}

buffer *chunkqueue_get_append_buffer(chunkqueue *cq) {
	chunk *c;
	
	c = chunkqueue_get_unused_chunk(cq);
	
	c->type = MEM_CHUNK;
	c->offset = 0;
	buffer_reset(c->data.mem);
	
	chunkqueue_append_chunk(cq, c);
	
	return c->data.mem;
}

off_t chunkqueue_length(chunkqueue *cq) {
	off_t len = 0;
	chunk *c;
	
	for (c = cq->first; c; c = c->next) {
		switch (c->type) {
		case MEM_CHUNK:
			len += c->data.mem->used ? c->data.mem->used - 1 : 0;
			break;
		case FILE_CHUNK:
			len += c->data.file.length;
			break;
		default:
			break;
		}
	}
	
	return len;
}

off_t chunkqueue_written(chunkqueue *cq) {
	off_t len = 0;
	chunk *c;
	
	for (c = cq->first; c; c = c->next) {
		switch (c->type) {
		case MEM_CHUNK:
		case FILE_CHUNK:
			len += c->offset;
			break;
		default:
			break;
		}
	}
	
	return len;
}

int chunkqueue_is_empty(chunkqueue *cq) {
	return cq->first ? 0 : 1;
}


