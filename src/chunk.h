#ifndef _CHUNK_H_
#define _CHUNK_H_

#include "buffer.h"
#include "array.h"

typedef struct chunkfile {
	unsigned int refcount;
	int fd;
	buffer *name; /* path of the file, may be NULL */
	int is_temp; /* file is temporary and will be deleted on cleanup */
} chunkfile;

typedef struct chunk {
	enum { MEM_CHUNK, FILE_CHUNK } type;

	buffer *mem; /* either the storage of the mem-chunk or the read-ahead buffer */

	/* filechunk */
	struct {
		chunkfile *fileref;
		off_t  start; /* starting offset in the file */
		off_t  length; /* octets to send from the starting offset */

		struct {
			char   *start; /* the start pointer of the mmap'ed area */
			size_t length; /* size of the mmap'ed area */
			off_t  offset; /* start is <n> octet away from the start of the file */
		} mmap;
	} file;

	/* octets sent from this chunk
	  the size of the chunk is either
	  - mem-chunk: buffer_string_length(mem)
	  - file-chunk: file.length
	*/
	off_t  offset;

	struct chunk *next;
} chunk;

typedef struct {
	chunk *first;
	chunk *last;

	chunk *unused;
	size_t unused_chunks;

	array *tempdirs;

	off_t  bytes_in, bytes_out;
} chunkqueue;

struct server;
struct connection;

chunkfile *chunkfile_new(int fd);
void chunkfile_acquire(chunkfile *cf);
void chunkfile_release(chunkfile **pcf);

chunkqueue *chunkqueue_init(void);
void chunkqueue_set_tempdirs(chunkqueue *cq, array *tempdirs);
void chunkqueue_append_file(chunkqueue *cq, buffer *fn, off_t offset, off_t len); /* copies "fn" */
void chunkqueue_append_chunkfile(chunkqueue *cq, chunkfile *cf, off_t offset, off_t len);
void chunkqueue_append_mem(chunkqueue *cq, const char *mem, size_t len); /* copies memory */
void chunkqueue_append_buffer(chunkqueue *cq, buffer *mem); /* may reset "mem" */
void chunkqueue_prepend_buffer(chunkqueue *cq, buffer *mem); /* may reset "mem" */

/* functions to handle buffers to read into: */
/* return a pointer to a buffer in *mem with size *len;
 *  it should be at least min_size big, and use alloc_size if
 *  new memory is allocated.
 * modifying the chunkqueue invalidates the memory area.
 * should always be followed by chunkqueue_get_memory(),
 *  even if nothing was read.
 * pass 0 for min_size/alloc_size for default values
 */
void chunkqueue_get_memory(chunkqueue *cq, char **mem, size_t *len, size_t min_size, size_t alloc_size);
/* append first len bytes of the memory queried with
 * chunkqueue_get_memory to the chunkqueue
 */
void chunkqueue_use_memory(chunkqueue *cq, size_t len);

/* mark first "len" bytes as written (incrementing chunk offsets)
 * and remove finished chunks
 */
void chunkqueue_mark_written(chunkqueue *cq, off_t len);

void chunkqueue_remove_finished_chunks(chunkqueue *cq);

void chunkqueue_steal(chunkqueue *dest, chunkqueue *src, off_t len);
int chunkqueue_steal_with_tempfiles(struct server *srv, chunkqueue *dest, chunkqueue *src, off_t len);

off_t chunkqueue_length(chunkqueue *cq);
void chunkqueue_free(chunkqueue *cq);
void chunkqueue_reset(chunkqueue *cq);

int chunkqueue_is_empty(chunkqueue *cq);

/* next chunk must be FILE_CHUNK. return values: -1 error, otherwise file descriptor (stored also in chunk) */
int chunkqueue_open_file(struct server *srv, struct connection *con, chunkqueue *cq);
int chunkqueue_open_trusted_file(struct server *srv, chunkqueue *cq); /* skip symlink checks */

#endif
