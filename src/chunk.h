#ifndef _CHUNK_H_
#define _CHUNK_H_

#include <glib.h>

#include "buffer.h"
#include "array.h"
#include "sys-mmap.h"

typedef struct chunk {
	enum { UNUSED_CHUNK, MEM_CHUNK, FILE_CHUNK } type;

	buffer *mem; /* either the storage of the mem-chunk or the read-ahead buffer */

	struct {
		/* filechunk */
		buffer *name; /* name of the file */
		off_t  start; /* starting offset in the file */
		off_t  length; /* octets to send from the starting offset */

		int    fd;
		struct {
			char   *start; /* the start pointer of the mmap'ed area */
			size_t length; /* size of the mmap'ed area */
			off_t  offset; /* start is <n> octets away from the start of the file */
		} mmap;

		int is_temp; /* file is temporary and will be deleted on cleanup */

		struct {
			int fd;
			off_t length;
			off_t offset;
		} copy;
	} file;

	off_t  offset; /* octets sent from this chunk
			  the size of the chunk is either
			  - mem-chunk: mem->used - 1
			  - file-chunk: file.length
			*/

	struct {
		off_t written;
		int ret_val;
	} async;

	struct chunk *next;
} chunk;

typedef struct {
	chunk *first;
	chunk *last;

	array *tempdirs;

	int is_closed;   /* the input to this CQ is closed */

	off_t  bytes_in, bytes_out;
} chunkqueue;

LI_API void chunkpool_free(void);

/* Chunk */

LI_API void chunk_free(chunk *c);
LI_API off_t chunk_length(chunk *c); /* remaining length */

LI_API gboolean chunk_is_done(chunk *c);
LI_API off_t chunk_set_done(chunk *c);


/* Chunk queue */

LI_API chunkqueue* chunkqueue_init(void);
LI_API void chunkqueue_free(chunkqueue *c);
LI_API void chunkqueue_reset(chunkqueue *c);

LI_API void chunkqueue_remove_finished_chunks(chunkqueue *cq);

/* Append/Preprend */
LI_API void chunkqueue_append_chunk(chunkqueue *cq, chunk *c);
LI_API void chunkqueue_prepend_chunk(chunkqueue *cq, chunk *c);

LI_API void chunkqueue_append_file(chunkqueue *c, buffer *fn, off_t offset, off_t len);

/* Copies mem */
LI_API void chunkqueue_append_mem(chunkqueue *c, const char *mem, size_t len);
LI_API void chunkqueue_prepend_mem(chunkqueue *c, const char *mem, size_t len);

/* Steals buffer */
LI_API void chunkqueue_append_buffer(chunkqueue *c, buffer *mem);
LI_API void chunkqueue_prepend_buffer(chunkqueue *c, buffer *mem);

/* Steals string */
LI_API void chunkqueue_append_gstring(chunkqueue *cq, GString *str);
LI_API void chunkqueue_prepend_gstring(chunkqueue *cq, GString *str);

/* Steal */
LI_API off_t chunkqueue_steal_all_chunks(chunkqueue *out, chunkqueue *in);
LI_API off_t chunkqueue_steal_len(chunkqueue *out, chunkqueue *in, off_t max_len);
LI_API off_t chunkqueue_steal_chunk(chunkqueue *out, chunk *c);
LI_API off_t chunkqueue_steal_tempfile(chunkqueue *out, chunk *in);

LI_API off_t chunkqueue_skip(chunkqueue *cq, off_t skip);
LI_API off_t chunkqueue_skip_all(chunkqueue *cq);

/* Tempfile */
LI_API void chunkqueue_set_tempdirs(chunkqueue *c, array *tempdirs);
LI_API chunk * chunkqueue_get_append_tempfile(chunkqueue *cq);

/* Status */
LI_API off_t chunkqueue_length(chunkqueue *c);
LI_API gboolean chunkqueue_is_empty(chunkqueue *c);

LI_API void chunkqueue_print(chunkqueue *cq);

LI_API off_t chunkqueue_to_buffer(chunkqueue *cq, buffer *b);
LI_API off_t chunkqeueu_to_gstring(chunkqueue *cq, GString *s);

LI_API off_t chunkqueue_to_buffer_len(chunkqueue *cq, buffer *b, off_t max_len);
LI_API off_t chunkqeueu_to_gstring_len(chunkqueue *cq, GString *s, off_t max_len);

#endif
