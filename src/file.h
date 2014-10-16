#ifndef _FILE_H_
#define _FILE_H_

#include "base.h"

#if defined HAVE_SYS_MMAN_H && defined HAVE_MMAP && defined ENABLE_MMAP
# include <setjmp.h>
# include <signal.h>
# define FILE_USE_MMAP
#endif

/* opens a regular file, checking symlink restrictions in the current connection context;
 * returns -1 on error, a valid file descriptor otherwise
 * on error errno will be EISDIR if the file is a directory (or the path ends in '/'),
 * EACCES if the symlink check failed or the system error
 */
int file_open(server *srv, connection *con, const buffer *filename, struct stat *st, int silent);

void file_get_mimetype(buffer *mimetype, connection *con, const buffer *filename, int fd);

typedef struct file_iterator {
	int fd;
	/* call file_iterator_release() before modifying start after _mmap/_load */
	off_t start; /* `buf` starts at `start` in file */
	/* read only */
	const unsigned char *buf;
	size_t length; /* length of `buf` */
	/* internal */
	size_t forward; /* `buf` - forward is "real" start of mmap()/allocated buffer */
	size_t alloc; /* 0: used mmap(), otherwise size of allocated buffer */
} file_iterator;


/* usage:

	BEGIN_FILE_CATCH_MMAP_FAIL
		-- handle mmap fail
		return;
	END_FILE_CATCH_MMAP_FAIL

	-- stuff that uses mmap()ed data, e.g:
	file_iterator_mmap(it, ...);
	-- use it->buf
	file_iterator_forward(it, ...);

	STOP_FILE_CATCH_MMAP_FAIL
*/

#ifdef FILE_USE_MMAP

/* internal stuff for use in macros */
extern volatile int file_sigbus_jmp_valid;
extern sigjmp_buf file_sigbus_jmp;
void file_sigbus_handler(int sig);

# define BEGIN_FILE_CATCH_MMAP_FAIL \
	file_sigbus_jmp_valid = 0; \
	if (0 != sigsetjmp(sigbus_jmp, 1)) { \
		file_sigbus_jmp_valid = 0;
# define END_FILE_CATCH_MMAP_FAIL \
	} \
	signal(SIGBUS, file_sigbus_handler); \
	file_sigbus_jmp_valid = 1;
# define STOP_FILE_CATCH_MMAP_FAIL \
	file_sigbus_jmp_valid = 0;

#else /* FILE_USE_MMAP */

# define BEGIN_FILE_CATCH_MMAP_FAIL \
	if (0) {
# define END_FILE_CATCH_MMAP_FAIL \
	}
# define STOP_FILE_CATCH_MMAP_FAIL \
	do { } while (0);

#endif /* FILE_USE_MMAP */

void file_iterator_init(file_iterator *it, int fd);
void file_iterator_clear(file_iterator *it);
/* release current buffer, keep other state */
void file_iterator_release(file_iterator *it);
/* read data from file
 * -1: error while reading (errno), 0: EOF, 1: success
 */
int file_iterator_load(file_iterator *it, size_t max_length);
/* makes data available with mmap() (if supported, otherwise fallbacks to file_iterator_load)
 * mmap()ed areas must be handled with care, as a resize on the underlying file can easily trigger
 *  a SIGBUS. use macros above to catch SIGBUS.
 * Note that a SIGBUG could happen anytime you access the mmap()ed area, so everything you do
 *  around it must keep the data consistent as far as cleanup is concerned.
 *  Better don't make any resource allocation/release in that block.
 * -1: error while mmap()ing (errno), 0: EOF (only if mmap is not supported), 1: success
 */
int file_iterator_mmap(file_iterator *it, size_t max_length);
/* marks `offset` bytes as read, adjusting data. doesn't read new data. */
void file_iterator_forward(file_iterator *it, size_t offset);

#endif
