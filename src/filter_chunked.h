#ifndef _FILTER_CHUNKED_H_
#define _FILTER_CHUNKED_H_

#include "filter.h"

gboolean filter_chunked_encode(chunkqueue *out, chunkqueue *in);

struct filter_chunked_decode_state;
typedef struct filter_chunked_decode_state filter_chunked_decode_state;

struct filter_chunked_decode_state* filter_chunked_decode_init();
void filter_chunked_decode_free(struct filter_chunked_decode_state *state);
gboolean filter_chunked_decode(chunkqueue *out, chunkqueue *in, struct filter_chunked_decode_state *state);

#endif
