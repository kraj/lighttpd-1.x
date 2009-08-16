#ifndef _CHUNK_HELPER_H_
#define _CHUNK_HELPER_H_

#include "base.h"
#include "log.h"

LI_API gboolean chunk_mmap(server *srv, connection *con, chunk *c, off_t we_want_max, char **data_start, off_t *data_len);
LI_API gboolean chunk_get_data(server *srv, connection *con, chunk *c, off_t we_want_max, char **data_start, off_t *data_len);

#endif
