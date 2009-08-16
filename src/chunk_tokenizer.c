#include "chunk_tokenizer.h"
#include "log.h"

LI_API gboolean chunk_tokenizer_init(chunk_tokenizer *t, chunkqueue *cq) {
	t->cq = cq;
	t->c = t->lookup_c = cq->first;
	t->cur = t->end = t->lookup_cur = t->lookup_end = NULL;
	t->lookup_offset = t->bytes_read = 0;
	chunk_tokenizer_prepare(t);
	return (cq != NULL);
}

LI_API void chunk_tokenizer_skip(chunk_tokenizer *t, off_t len) {
	off_t we_have;
	t->lookup_offset = 0;

	if (!t->c) return;

	we_have = t->end - t->cur;
	if (we_have > len) we_have = len;
	len -= we_have;
	t->bytes_read += we_have;
	if (0 == len) {
		t->cur += we_have;
	} else {
		t->c = t->c->next;
	}

	for ( ; t->c && (len > 0); t->c = t->c->next) {
		we_have = chunk_length(t->c);
		if (!we_have) continue;
		if (we_have > len) we_have = len;
		len -= we_have;
		t->bytes_read += we_have;
		if (!len) {
			if (!_chunk_tokenizer_frame(t->c, &t->cur, &t->end)) goto error;
			t->cur += we_have;
			break;
		}
	}

	if (len > 0) goto error;

	t->lookup_cur = t->cur;
	t->lookup_end = t->end;
	return;

error:
	ERROR("Couldn't skip %jd bytes, not enough chunks", (intmax_t) len);
	t->cur = t->end = t->lookup_cur = t->lookup_end = NULL;
}

LI_API void chunk_tokenizer_read_buffer(chunk_tokenizer *t, off_t len, buffer* b) {
	off_t we_have;
	t->lookup_offset = 0;
	buffer_prepare_append(b, len);

// 	TRACE("Get %jd bytes", (intmax_t) len);
	if (!t->c) return;

	we_have = t->end - t->cur;
	if (we_have > len) we_have = len;
	len -= we_have;
	t->bytes_read += we_have;
	buffer_append_string_len(b, t->cur, we_have);
	t->cur += we_have;
	if (len > 0) {
		t->c = t->c->next;
	}

	for ( ; t->c && (len > 0); t->c = t->c->next) {
		we_have = chunk_length(t->c);
		if (!we_have) continue;
		if (we_have > len) we_have = len;
// 		TRACE("we_have: %jd", (intmax_t) we_have);
		len -= we_have;
		t->bytes_read += we_have;
		switch (t->c->type) {
		case MEM_CHUNK:
			buffer_append_string_len(b,  &t->c->mem->ptr[t->c->offset], we_have);
			break;
// 		case GSTRING_CHUNK:
// 			buffer_append_string_len(b, &t->c->str->str[t->c->offset], we_have);
// 			break;
		case FILE_CHUNK:
		case UNUSED_CHUNK: // unused chunks should have length 0, so continue should have triggered
			goto error;
		}
		if (!len) {
			if (!_chunk_tokenizer_frame(t->c, &t->cur, &t->end)) goto error;
			t->cur += we_have;
			break;
		}
	}

	if (len > 0) goto error;

	t->lookup_cur = t->cur;
	t->lookup_end = t->end;
// 	TRACE("got '%s'", BUF_STR(b));
	return;

error:
	ERROR("Couldn't skip %jd bytes, not enough chunks", (intmax_t) len);
	t->cur = t->end = t->lookup_cur = t->lookup_end = NULL;
}
