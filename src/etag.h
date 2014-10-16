#ifndef ETAG_H
#define ETAG_H

#include "buffer.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { ETAG_USE_INODE = 1, ETAG_USE_MTIME = 2, ETAG_USE_SIZE = 4 } etag_flags_t;

int etag_is_equal(buffer *etag, const char *matches, int weak_ok);
int etag_create(buffer *etag, struct stat *st, etag_flags_t flags);
int etag_mutate(buffer *mut, buffer *etag);

/* create + mutate */
int etag_build(buffer *etag, struct stat *st, etag_flags_t flags);

#endif
