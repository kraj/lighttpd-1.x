
#include "base.h"
#include "filter_chunked.h"
#include "chunk_tokenizer.h"

#include <assert.h>
#include <glib.h>

static int http_chunk_append_len(chunkqueue *cq, size_t len) {
	size_t i, olen = len, j;
	buffer *b;

	b = buffer_init();

	if (len == 0) {
		buffer_copy_string(b, "0");
	} else {
		for (i = 0; i < 8 && len; i++) {
			len >>= 4;
		}

		/* i is the number of hex digits we have */
		buffer_prepare_copy(b, i + 1);

		for (j = i-1, len = olen; j+1 > 0; j--) {
			b->ptr[j] = (len & 0xf) + (((len & 0xf) <= 9) ? '0' : 'a' - 10);
			len >>= 4;
		}
		b->used = i;
		b->ptr[b->used++] = '\0';
	}

	buffer_append_string(b, "\r\n");
	chunkqueue_append_mem(cq, CONST_BUF_LEN(b));
	len = b->used - 1;

	buffer_free(b);

	return len;
}

// removes finished chunks
static void filter_chunked_encode_len(chunkqueue *out, chunkqueue *in, off_t total) {
	http_chunk_append_len(out, total);
	assert(total > 0);
	assert(total == chunkqueue_steal_len(out, in, (size_t) total)); // removes finished chunks
	chunkqueue_append_mem(out, CONST_STR_LEN("\r\n"));
	out->bytes_in += 2;
}

// #define MAX_HTTP_CHUNK_SIZE ((off_t) ((1 << 31) - 1))
#define MAX_HTTP_CHUNK_SIZE ((off_t) (INT32_MAX))
gboolean filter_chunked_encode(chunkqueue *out, chunkqueue *in) {
	chunk *c;
	off_t total = 0, len;

	if (out->is_closed) {
		if (chunkqueue_length(in)) return FALSE;
		return TRUE;
	}

	// try to not split chunks, but take as many as available into one http-"chunk"
	chunkqueue_remove_finished_chunks(in);
	for (c = in->first; c; c = in->first) {
		for ( ; c ; c = c->next ) {
			len = chunk_length(c);
			if (MAX_HTTP_CHUNK_SIZE - total < len) {
				if (total == 0) {
					total = MAX_HTTP_CHUNK_SIZE; // split a chunk as it is too big
				}
				filter_chunked_encode_len(out, in, total);
				total = 0;
				break; // restart outer loop (c != 0, so break below does not trigger)
			}
			total += len;
		}
		if (!c) break; // end of chunkqueue reached, remaining fits into one http-"chunk"
	}

	if (total != 0) {
		filter_chunked_encode_len(out, in, total);
	}

	if (in->is_closed) {
		chunkqueue_append_mem(out, CONST_STR_LEN("0\r\n\r\n"));
		out->bytes_in += 5;
		out->is_closed = 1;
	}

	return TRUE;
}

struct filter_chunked_decode_state {
	enum { FCDS_LEN, FCDS_MORE_LEN, FCDS_EXTENSION, FCDS_DATA, FCDS_END } st;
	off_t chunklen;
	GString *lenbuf;
	gboolean had_cr, is_empty_line;
};

filter_chunked_decode_state* filter_chunked_decode_init() {
	filter_chunked_decode_state *state = calloc(1, sizeof(filter_chunked_decode_state));

	state->lenbuf = g_string_sized_new(0);

	return state;
}

void filter_chunked_decode_free(filter_chunked_decode_state *state) {
	if (!state) return;
	g_string_free(state->lenbuf, TRUE);
	free(state);
}

static int hexchar_to_int(unsigned char c) {
	if ('0' <= c && c <= '9')
		return c - '0';
	else if ('a' <= c && c <= 'f')
		return c - 'a' + 10;
	else if ('A' <= c && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* max value before last shift << 4 must not have bits in the mask: 1111 1000 0000 0000 .... 0000
 * (keeps sign bit on zero and does not shift any bits out)
 */
#define OFF_MASK (((off_t) 31) << (8*SIZEOF_OFF_T - 3))

/* Returns TRUE if more data is needed or all data is processed
 * If all data is processed and no more data is needed for a valid content,
 * the out chunkqueue gets closed.
 * Returns FALSE otherwise, i.e. if an error occured.
 * DO NOT CALL AGAIN after an error.
 */
#define GET(t, c) \
	if (chunk_tokenizer_eof(&t)) return TRUE; /* Need more data */ \
	if (!chunk_tokenizer_get(&t, &c)) return FALSE; /* Error */ \
	t.c->offset++; t.cq->bytes_out++;

gboolean filter_chunked_decode(chunkqueue *out, chunkqueue *in, filter_chunked_decode_state *state) {
	unsigned char c;
	int i;
	chunk_tokenizer t;

	if (out->is_closed) {
		if (chunkqueue_length(in)) return FALSE;
		return TRUE;
	}

	chunk_tokenizer_init(&t, in);

	for ( ; ; ) {
		if (state->st == FCDS_DATA) {
			if (state->chunklen) {
				off_t we_have = chunkqueue_steal_len(out, in, state->chunklen);
				if (!we_have) return TRUE;
				state->chunklen -= we_have;
			}
			if (!state->chunklen) {
				if (!state->had_cr) {
					GET(t, c);
					if (c != '\r') return FALSE;
					state->had_cr = 1;
				}
				GET(t, c);
				if (c != '\n') return FALSE;
				state->had_cr = 0;
				state->st = FCDS_LEN;
			}
			continue;
		}

		switch (state->st) {
			case FCDS_LEN:
				state->had_cr = 0;

				GET(t, c);
				i = hexchar_to_int(c);
				if (i == -1) {
					ERROR("Expected hex character, got '%c' (0x%xd)", c, c);
					return FALSE;
				}
				state->chunklen = i;
				state->st = FCDS_MORE_LEN;
				// Fall through
// 				break;
			case FCDS_MORE_LEN:
				for ( ; ; ) {
					gboolean had_cr = state->had_cr;
					state->had_cr = 0;

					GET(t, c);
					i = hexchar_to_int(c);
					if (had_cr) {
						if (c != '\n') {
							ERROR("Expected '\\n', got '%c' (0x%xd)", c, c);
							return FALSE;
						}
						if (!state->chunklen) {
							state->is_empty_line = 1;
							state->st = FCDS_END;
						} else {
							state->st = FCDS_DATA;
						}
						break; // Stop parsing chunk length
					}
					if (i == -1) { // Not a hex digit
						if (c == ';') {
							state->st = FCDS_EXTENSION;
							break; // Stop parsing chunk length
						} else if (c == '\r') {
							state->had_cr = 1;
						} else {
							ERROR("Expected hex character or ';' or '\\r', got '%c' (0x%xd)", c, c);
							return FALSE;
						}
					} else {       // hex digit
						if (0 != (state->chunklen && OFF_MASK)) {
							ERROR("HTTP chunk too big for off_t (sizeof(off_t) = %zu)", SIZEOF_OFF_T);
							return FALSE;
						}
						state->chunklen = (state->chunklen << 4) + i;
					}
				}
				break;
			case FCDS_EXTENSION:
				// This is not rfc-conform: the standard allows CR / LF characters in quoted extension values
				// We just search for CR LF, regardless of quotes, and ignore all other characters.
				// CR without following LF is not allowed
				for ( ; ; ) {
					gboolean had_cr = state->had_cr;
					state->had_cr = 0;

					GET(t, c);
					if (had_cr) {
						if (c != '\n') {
							ERROR("Expected '\\n', got '%c' (0x%xd)", c, c);
							return FALSE;
						}
						state->st = FCDS_DATA;
						break; // Stop parsing extension
					}
					if (c == '\r') {
						state->had_cr = 1;
					}
				}
				break;
			case FCDS_END:
				/* We just search for CR LF CR LF, regardless of quotes, and ignore all other characters.
				 * CR without following LF is not allowed
				 */
				state->is_empty_line = 1;
				for ( ; ; ) {
					gboolean had_cr = state->had_cr;
					state->had_cr = 0;

					GET(t, c);
					if (had_cr) {
						if (c != '\n') {
							ERROR("Expected '\\n', got '%c' (0x%xd)", c, c);
							return FALSE;
						}
						if (state->is_empty_line) goto finish; /* Got CR LF CR LF */
						state->is_empty_line = 1;
					} else if (c == '\r') {
						state->had_cr = 1;
					} else { /* Not an empty line */
						state->is_empty_line = 0;
					}
				}
				break;
			case FCDS_DATA: /* not possible */
				break;
		}
	}

finish:
	out->is_closed = TRUE;
	return TRUE;
}
