#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "file_cache.h"
#include "connections.h"
#include "plugin.h"
#include "joblist.h"

#include "network_backends.h"
#include "sys-mmap.h"
#include "sys-socket.h"
#include "file_descr_funcs.h"

#ifdef USE_OPENSSL
# include <openssl/ssl.h> 
# include <openssl/err.h> 
# include <openssl/rand.h> 
#endif


network_t network_write_chunkqueue(server *srv, file_descr *write_fd, chunkqueue *cq) {
	network_t ret = NETWORK_UNSET;
	chunk *c, *pc = NULL;
	
#ifdef TCP_CORK	
	/* Linux: put a cork into the socket as we want to combine the write() calls */
	if (write_fd->is_socket) {
		int i = 1;
		setsockopt(write_fd->fd, IPPROTO_TCP, TCP_CORK, &i, sizeof(i));
	}
#endif

	ret = ((*write_fd->write_func)(srv, write_fd, cq));

	switch (ret) {
	case NETWORK_OK:
		for (c = cq->first; c; c = c->next) {
			if ((c->type == FILE_CHUNK && c->offset == c->data.file.length) ||
			    (c->type == MEM_CHUNK && c->offset == (off_t)c->data.mem->used - 1)) {
				/* chunk is finished */
				buffer_reset(c->data.mem);

				/* remember the last finished chunk */
				pc = c;
			}
		}
		
		if (c) {
			/* there is still something to write */
			
			if (c != cq->first) {
				/* move the first few buffers to unused */
				
				assert(pc);

				/*
				 * move all finished chunks to unused
				 * move the last used chunk to first
				 *
				 */
				
				pc->next = cq->unused;
				cq->unused = cq->first;
				cq->first = c;
			}
		} else {
			/* everything is written */
			chunkqueue_reset(cq);

			ret = NETWORK_QUEUE_EMPTY;
		}
	default:
		break;
	}
	
#ifdef TCP_CORK
	if (write_fd->is_socket) {
		int i = 0;
		setsockopt(write_fd->fd, IPPROTO_TCP, TCP_CORK, &i, sizeof(i));
	}
#endif

	return ret;
}

network_t network_read_chunkqueue(server *srv, file_descr *read_fd, chunkqueue *cq) {
	network_t ret;

	ret = ((*read_fd->read_func)(srv, read_fd, cq));

	return ret;
}
