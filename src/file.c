#include "file.h"
#include "log.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <errno.h>
#include <string.h>

#ifdef O_NOFOLLOW
static int check_no_symlink_file_open(server *srv, connection *con, const buffer *filename, int silent) {
	int parent;
	int fd;
	int save_errno = 0;
	const int flags
		= O_RDONLY | O_NOFOLLOW
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

	if ('\0' == current[0] || 0 == strcmp(current, ".") || 0 == strcmp(current, "..")) {
		save_errno = 403;
		/* this shouldn't happen: filenames shouldn't end in '/', '/.' or '/..': log it */
		if (!silent) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "filename ends in a directory:", filename);
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
	if (0 != save_errno) save_errno = errno;
	if (-1 != parent) close(parent);
	buffer_free(path);
	errno = save_errno;
	return -1;
}
#endif

int file_open(server *srv, connection *con, const buffer *filename, struct stat *st, int silent) {
	struct stat local_st;
	int fd = -1;
	int save_errno = 0;
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

	if (NULL == st) st = &local_st;

	if (buffer_string_is_empty(filename)) {
		save_errno = ENOENT;
		if (!silent) {
			log_error_write(srv, __FILE__, __LINE__, "s", "cannot open empty filename");
		}
		goto error;
	}

	if (!con->conf.follow_symlink) {
		fd = check_no_symlink_file_open(srv, con, filename, silent);
		if (-1 == fd) goto error;
	} else {
		fd = open(filename->ptr, flags);
		if (-1 == fd) {
			save_errno = errno;
			if (!silent && con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "sbss", "couldn't open file:", filename, ":", strerror(errno));
			}
			goto error;
		}
	}

	if (-1 == fstat(fd, st)) {
		save_errno = errno;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "couldn't stat file:", filename, ":", strerror(errno));
		}
		goto error;
	}

	if (!S_ISREG(st->st_mode)) {
		save_errno = EACCES;
		if (!silent && con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "sb", "not a regular file:", filename);
		}
		goto error;
	}

	return fd;

error:
	if (0 != save_errno) save_errno = errno;
	if (-1 != fd) close(fd);
	errno = save_errno;
	return -1;
}
