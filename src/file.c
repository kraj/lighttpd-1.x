#include "file.h"
#include "log.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <unistd.h>

#include <errno.h>
#include <string.h>

#ifdef HAVE_ATTR_ATTRIBUTES_H
# include <attr/attributes.h>
#endif

#ifdef HAVE_SYS_EXTATTR_H
# include <sys/extattr.h>
#endif

#ifdef FILE_USE_MMAP
# include <sys/mman.h>
#endif

static const int open_read_regular_flags
	= O_RDONLY
#ifdef O_BINARY
	| O_BINARY
#endif
#ifdef O_NONBLOCK
	| O_NONBLOCK /* don't block on opening named pipes */
#endif
#ifdef O_NOCTTY
	| O_NOCTTY /* don't allow overtaking controlling terminal */
#endif
;

static int simple_file_open(server *srv, connection *con, const buffer *filename, int silent) {
	int fd = open(filename->ptr, open_read_regular_flags);
	if (-1 == fd) {
		int save_errno = errno;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "couldn't open file:", filename, ":", strerror(errno));
		}
		errno = save_errno;
		return -1;
	}

	return fd;
}

#if defined(O_NOFOLLOW) && 0
static int check_no_symlink_file_open(server *srv, connection *con, const buffer *filename, int silent) {
	int parent;
	int fd;
	int save_errno = 0;
	const int flags = open_read_regular_flags | O_NOFOLLOW;
	const int dirflags = O_DIRECTORY | O_NOFOLLOW
#ifdef O_SEARCH
		| O_SEARCH
#endif
	;
	buffer *path = buffer_init_buffer(filename);
	char *current, *next;

	if ('/' == path->ptr[0]) {
		parent = open("/", dirflags);
		current = path->ptr + 1;
	} else {
		parent = open(".", dirflags);
		current = path->ptr;
	}
	if (-1 == parent) {
		save_errno = errno;
		/* this shouldn't happen: log it */
		if (!silent) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "couldn't open base directory:", strerror(errno));
		}
		goto error;
	}

	while (NULL != (next = strchr(current, '/'))) {
		*next = '\0';

		if (0 == strcmp(current, "..")) {
			save_errno = EACCES;
			/* this shouldn't happen (path-simplify should catch it): log it */
			if (!silent) {
				log_error_write(srv, __FILE__, __LINE__, "sb", "detected directory traversal in:", filename);
			}
			goto error;
		}
		if ('\0' == current[0] || 0 == strcmp(current, ".")) {
			/* '' or '.': skip */
			current = next + 1;
			continue;
		}

		fd = openat(parent, current, dirflags);
		if (-1 == fd) {
			save_errno = errno;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sssbss", "couldn't open directory:", current, "in:", filename, ":", strerror(errno));
			}
			goto error;
		}

		close(parent);
		parent = fd;
		fd = -1;

		current = next + 1;
	}

	if ('\0' == current[0]) {
		save_errno = EISDIR;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "filename ends in a '/':", filename);
		}
		goto error;
	}

	if ('\0' == current[0] || 0 == strcmp(current, ".") || 0 == strcmp(current, "..")) {
		save_errno = EISDIR;
		/* this shouldn't happen: filenames shouldn't end in '/.' or '/..': log it */
		if (!silent) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "filename ends in a '/.' or '/..':", filename);
		}
		goto error;
	}

	fd = openat(parent, current, flags);
	if (-1 == fd) {
		save_errno = errno;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "couldn't open file:", filename, ":", strerror(errno));
		}
		goto error;
	}

	buffer_free(path);
	close(parent);

	return fd;

error:
	if (0 == save_errno) save_errno = errno;
	if (-1 != parent) close(parent);
	buffer_free(path);
	errno = save_errno;
	return -1;
}

#elif defined(HAVE_LSTAT)
/* O_NOFOLLOW not available, but have lstat: */

/* vulnerable to race-condition between check for symlink and usage; without
 * O_NOFOLLOW this just isn't possible
 */
static int check_no_symlink_file_open(server *srv, connection *con, const buffer *filename, int silent) {
	int fd = -1;
	int save_errno = 0;
	const int flags = open_read_regular_flags;
	buffer *path = buffer_init_buffer(filename);
	char *current, *next;

	if ('/' == path->ptr[0]) {
		current = path->ptr + 1;
	} else {
		current = path->ptr;
	}

	while (NULL != (next = strchr(current, '/'))) {
		struct stat st;
		*next = '\0';

		if (0 == strcmp(current, "..")) {
			save_errno = EACCES;
			/* this shouldn't happen (path-simplify should catch it): log it */
			if (!silent) {
				log_error_write(srv, __FILE__, __LINE__, "sb", "detected directory traversal in:", filename);
			}
			goto error;
		}
		if ('\0' == current[0] || 0 == strcmp(current, ".")) {
			/* '' or '.': skip */
			current = next + 1;
			continue;
		}

		if (-1 == lstat(path->ptr, &st)) {
			save_errno = errno;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sss", "lstat failed for:", path->ptr, strerror(errno));
			}
			goto error;
		}

		if (S_ISLNK(st.st_mode)) {
			save_errno = EACCES;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sss", "found symlink:", path->ptr);
			}
			goto error;
		}

		if (!S_ISDIR(st.st_mode)) {
			save_errno = ENOTDIR;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sss", "not a directory:", path->ptr);
			}
			goto error;
		}

		*next = '/';
		current = next + 1;
	}

	if ('\0' == current[0]) {
		save_errno = EISDIR;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "filename ends in a '/':", filename);
		}
		goto error;
	}

	if ('\0' == current[0] || 0 == strcmp(current, ".") || 0 == strcmp(current, "..")) {
		save_errno = EISDIR;
		/* this shouldn't happen: filenames shouldn't end in '/.' or '/..': log it */
		if (!silent) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "filename ends in a '/.' or '/..':", filename);
		}
		goto error;
	}

	{
		struct stat st;
		if (-1 == lstat(filename->ptr, &st)) {
			save_errno = errno;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sss", "lstat failed for:", filename->ptr, strerror(errno));
			}
			goto error;
		}

		if (S_ISLNK(st.st_mode)) {
			save_errno = EACCES;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sss", "found symlink:", filename->ptr);
			}
			goto error;
		}
	}

	fd = open(path->ptr, flags);
	if (-1 == fd) {
		save_errno = errno;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "couldn't open file:", filename, ":", strerror(errno));
		}
		goto error;
	}

	buffer_free(path);

	return fd;

error:
	if (0 == save_errno) save_errno = errno;
	buffer_free(path);
	errno = save_errno;
	return -1;
}

#else

/* no symlink support - assume all files are "safe" */
static int check_no_symlink_file_open(server *srv, connection *con, const buffer *filename, int silent) {
	return simple_file_open(srv, con, filename, silent);
}

#endif

int file_open(server *srv, connection *con, const buffer *filename, struct stat *st, int silent) {
	struct stat local_st;
	int fd = -1;
	int save_errno = 0;

	if (NULL == st) st = &local_st;

	if (buffer_string_is_empty(filename)) {
		save_errno = ENOENT;
		if (!silent) {
			log_error_write(srv, __FILE__, __LINE__, "s", "cannot open empty filename");
		}
		goto error;
	}
	if ('/' == filename->ptr[buffer_string_length(filename) - 1]) {
		save_errno = EISDIR;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "filename ends in '/':", filename);
		}
		goto error;
	}

	if (!con->conf.follow_symlink) {
		fd = check_no_symlink_file_open(srv, con, filename, silent);
		if (-1 == fd) goto error;
	} else {
		fd = simple_file_open(srv, con, filename, silent);
		if (-1 == fd) goto error;
	}

	if (-1 == fstat(fd, st)) {
		save_errno = errno;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "couldn't fstat file:", filename, ":", strerror(errno));
		}
		goto error;
	}

	if (!S_ISREG(st->st_mode)) {
		save_errno = S_ISDIR(st->st_mode) ? EISDIR : EACCES;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "not a regular file:", filename);
		}
		goto error;
	}

	return fd;

error:
	if (0 == save_errno) save_errno = errno;
	if (-1 != fd) close(fd);
	errno = save_errno;
	return -1;
}


#if defined(HAVE_XATTR)

static void stat_cache_attr_get(buffer *mimetype, int fd) {
	int attrlen = 1023;
	buffer_string_prepare_copy(mimetype, attrlen);
	if (0 == attr_getf(fd, "Content-Type", mimetype->ptr, &attrlen, 0)) {
		buffer_string_set_length(mimetype, attrlen);
	} else {
		buffer_string_set_length(mimetype, 0);
	}
}

#elif defined(HAVE_EXTATTR)

static int stat_cache_attr_get(buffer *buf, int fd) {
	ssize_t attrlen = 1023;
	buffer_string_prepare_copy(buf, attrlen);
	if (-1 != (attrlen = extattr_get_fd(fd, EXTATTR_NAMESPACE_USER, "Content-Type", buf->ptr, attrlen))) {
		buffer_string_set_length(mimetype, attrlen);
	} else {
		buffer_string_set_length(mimetype, 0);
	}
}

#endif

void file_get_mimetype(buffer *mimetype, connection *con, const buffer *filename, int fd) {
	size_t k, namelen;

#if defined(HAVE_XATTR) || defined(HAVE_EXTATTR)
	if (con->conf.use_xattr) {
		stat_cache_attr_get(mimetype, fd);
		if (!buffer_string_is_empty(mimetype)) return; /* found mimetype */
	}
#else
	UNUSED(fd);
#endif

	/* xattr did not set a content-type. ask the config */
	namelen = buffer_string_length(filename);

	for (k = 0; k < con->conf.mimetypes->used; k++) {
		data_string *ds = (data_string *)con->conf.mimetypes->data[k];
		buffer *type = ds->key;
		size_t typelen = buffer_string_length(type);

		if (buffer_is_empty(type)) continue;

		/* check if the right side is the same */
		if (typelen > namelen) continue;

		if (0 == strncasecmp(filename->ptr + namelen - typelen, type->ptr, typelen)) {
			buffer_copy_buffer(mimetype, ds->value);
			return;
		}
	}

	buffer_string_set_length(mimetype, 0);
}

#ifdef FILE_USE_MMAP
volatile int file_sigbus_jmp_valid;
sigjmp_buf file_sigbus_jmp;

void file_sigbus_handler(int sig) {
	UNUSED(sig);
	if (file_sigbus_jmp_valid) siglongjmp(file_sigbus_jmp, 1);
	log_failed_assert(__FILE__, __LINE__, "SIGBUS");
}
#endif

static size_t get_pagesize(void) {
	static size_t pagesize;
	if (0 == pagesize) {
		pagesize = sysconf(_SC_PAGE_SIZE);
		force_assert(0 != pagesize);
		force_assert(0 == (pagesize & (pagesize-1))); /* must be a power of 2 */
	}
	return pagesize;
}

void file_iterator_init(file_iterator *it, int fd) {
	memset(it, 0, sizeof(*it));
	it->fd = fd;
}

void file_iterator_release(file_iterator *it) {
	if (NULL != it->buf) {
		const unsigned char *real_buf = it->buf - it->forward;
		size_t real_length = it->length + it->forward;
		if (0 == it->alloc) {
#ifdef FILE_USE_MMAP
			munmap(real_buf, real_length);
#else
			log_failed_assert(__FILE__, __LINE__, "mmap not enabled, can't munmap");
#endif
		} else {
			UNUSED(real_length);
			free((void*) real_buf);
			it->alloc = 0;
		}
		it->buf = NULL;
		it->forward = 0;
		it->length = 0;
	} else {
		force_assert(0 == it->length);
		force_assert(0 == it->forward);
		force_assert(0 == it->alloc);
	}
}

void file_iterator_clear(file_iterator *it) {
	file_iterator_release(it);
	it->fd = -1;
	it->start = 0;
}

int file_iterator_load(file_iterator *it, size_t max_length) {
	ssize_t r;

	if (NULL != it->buf && 0 == it->alloc) {
		/* was mmap() - release and read() instead */
		file_iterator_release(it);
	}

	/* still got some data from previous load? */
	if (it->length > 0) return 1;

	force_assert(max_length > 0);

	if (-1 == lseek(it->fd, it->start, SEEK_SET)) {
		return -1;
	}
	if (max_length > it->alloc) {
		/* resize buffer */
		const size_t pagesize = get_pagesize();

		if (NULL != it->buf) free((char*) it->buf);
		it->alloc = (max_length + pagesize - 1) & ~(pagesize - 1); /* align at pagesize */
		force_assert(max_length <= it->alloc);
		it->buf = malloc(it->alloc);
		if (NULL == it->buf) {
			it->alloc = 0;
			return -1;
		}
	}
	if (0 > (r = read(it->fd, (char*) it->buf, max_length))) return -1;

	it->length = r;
	return r > 0 ? 1 : 0;
}

int file_iterator_mmap(file_iterator *it, size_t max_length) {
#ifdef FILE_USE_MMAP
	if (it->length > 0) return 1;

	if (it->alloc > 0) {
		/* free allocated buffer */
		file_iterator_release(it);
	}

	force_assert(max_length > 0);
	{
		off_t start = it->start & ~(get_pagesize() - 1); /* align at pagesize */
		void *ptr;
		size_t forward, len;

		forward = it->start - start;
		len = max_length  + forward;
		force_assert(len > forward);

		ptr = mmap(NULL, len, PROT_READ, MAP_SHARED, it->fd, start);
		if (MAP_FAILED == ptr) return -1;
		force_assert(NULL != ptr);

		it->buf = ptr + forward;
		it->length = len - forward;
		it->forward = forward;
	}

	return 1;
#else
	return file_iterator_load(it, max_length);
#endif
}

void file_iterator_forward(file_iterator *it, size_t offset) {
	if (offset >= it->length) {
		if (0 == it->alloc) {
			/* mmap(): release */
			file_iterator_release(it);
		} else {
			/* reset pointer to start of allocated buffer */
			it->buf = it->buf - it->forward;
			it->length = 0;
			it->forward = 0;
		}
	} else {
		it->forward += offset;
		it->buf += offset;
		it->length -= offset;
	}
	it->start += offset;
}
