#ifndef _CHUNK_TOKENIZER_H_
#define _CHUNK_TOKENIZER_H_

#include "base.h"
#include "log.h"

struct chunk_tokenizer {
	chunkqueue *cq;

	/* internal state */
	chunk *c;
	char *cur, *end;
	chunk *lookup_c;
	char *lookup_cur, *lookup_end;
	off_t lookup_offset, bytes_read;
};
typedef struct chunk_tokenizer chunk_tokenizer;

LI_API gboolean chunk_tokenizer_init(chunk_tokenizer *t, chunkqueue *cq);
LI_API void chunk_tokenizer_skip(chunk_tokenizer *t, off_t len);
LI_API void chunk_tokenizer_read_buffer(chunk_tokenizer *t, off_t len, buffer* b);

static inline gboolean chunk_tokenizer_prepare(chunk_tokenizer *t);
static inline gboolean chunk_tokenizer_lookup(chunk_tokenizer *t, unsigned char *c);
static inline gboolean chunk_tokenizer_get(chunk_tokenizer *t, unsigned char *c);

static inline gboolean chunk_tokenizer_eof(chunk_tokenizer *t);
static inline gboolean chunk_tokenizer_lookup_eof(chunk_tokenizer *t);

/* Inlines */

static inline gboolean _chunk_tokenizer_frame(chunk *c, char **cur, char **end) {
	*cur = *end = NULL;
	if (!c) return TRUE;
	switch (c->type) {
	case MEM_CHUNK:
		*cur = &c->mem->ptr[c->offset];
		*end = &c->mem->ptr[c->mem->used - 1];
		return TRUE;
// 	case GSTRING_CHUNK:
// 		*cur = &c->str->str[c->offset];
// 		*end = &c->str->str[c->str->len];
// 		return TRUE;
	case FILE_CHUNK:
		ERROR("%s", "File chunks are not supported");
		return FALSE;
	case UNUSED_CHUNK:
		ERROR("%s", "Unused chunk - should never happen");
		return FALSE;
	}
	ERROR("%s", "Unknown chunktype - should never happen");
	return FALSE;
}

static inline gboolean chunk_tokenizer_prepare(chunk_tokenizer *t) {
	gboolean first = (t->cur == NULL);
	if (!first && !t->c) return TRUE; // EOF
	if (t->cur >= t->end) {
		if (!first) {
			t->c = t->c->next;
		}
		t->cur = t->end = t->lookup_cur = t->lookup_end = NULL;

		while (t->c && chunk_is_done(t->c)) t->c = t->c->next;
		t->lookup_c = t->c;
		if (!t->c) return TRUE; // EOF

		if (!_chunk_tokenizer_frame(t->c, &t->cur, &t->end)) return FALSE; // ERROR
		t->lookup_c = t->c;
		t->lookup_cur = t->cur;
		t->lookup_end = t->end;
		t->lookup_offset = 0;
	} else if (t->lookup_cur >= t->lookup_end) {
		t->lookup_cur = t->lookup_end = NULL;
		if (!t->lookup_c) return TRUE; // EOF
		t->lookup_c = t->lookup_c->next; // step at least one chunk
		while (t->lookup_c && chunk_is_done(t->lookup_c)) t->lookup_c = t->lookup_c->next;
		if (!t->lookup_c) return TRUE; // EOF
		if (!_chunk_tokenizer_frame(t->lookup_c, &t->lookup_cur, &t->lookup_end)) return FALSE; // ERROR
	}

	return TRUE;
}

static inline gboolean chunk_tokenizer_lookup(chunk_tokenizer *t, unsigned char *c) {
	if (!chunk_tokenizer_prepare(t)) return FALSE;
	if (!t->lookup_cur) return FALSE; // EOF

	*c = *t->lookup_cur++;
	t->lookup_offset++;

	return TRUE;
}

static inline gboolean chunk_tokenizer_get(chunk_tokenizer *t, unsigned char *c) {
	if (!chunk_tokenizer_prepare(t)) return FALSE;
	if (!t->cur) return FALSE; // EOF

	*c = *t->cur;
	t->bytes_read++;

	if (t->cur++ != t->lookup_cur++) {
		t->lookup_c = t->c;
		t->lookup_cur = t->cur;
		t->lookup_end = t->end;
		t->lookup_offset = 0;
	}

	return TRUE;
}

static inline gboolean chunk_tokenizer_eof(chunk_tokenizer *t) {
	if (!chunk_tokenizer_prepare(t)) return FALSE; // ERROR
	if (!t->cur) return TRUE;
	return FALSE;
}

static inline gboolean chunk_tokenizer_lookup_eof(chunk_tokenizer *t) {
	if (!chunk_tokenizer_prepare(t)) return FALSE; // ERROR
	if (!t->lookup_cur) return TRUE;
	return FALSE;
}

#endif
