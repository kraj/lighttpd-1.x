#ifndef _FILE_H_
#define _FILE_H_

#include "base.h"

/* opens a regular file, checking symlink restrictions in the current connection context;
 * returns -1 on error, a valid file descriptor otherwise
 */
int file_open(server *srv, connection *con, const buffer *filename, struct stat *st, int silent);

#endif
