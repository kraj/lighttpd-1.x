#ifndef _MOD_PROXY_CORE_H_
#define _MOD_PROXY_CORE_H_

#include "buffer.h"
#include "base.h"

#define PROXY_BACKEND_CONNECT_PARAMS \
	(server *srv, connection *con, void *p_d)

#define PROXY_BACKEND_CONNECT_RETVAL handler_t

#define PROXY_BACKEND_CONNECT(name) \
	PROXY_BACKEND_CONNECT_RETVAL name PROXY_BACKEND_CONNECT_PARAMS

#define PROXY_BACKEND_CONNECT_PTR(name) \
	PROXY_BACKEND_CONNECT_RETVAL (* name)PROXY_BACKEND_CONNECT_PARAMS

#endif
