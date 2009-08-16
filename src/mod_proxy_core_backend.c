#include <stdlib.h>

#include "mod_proxy_core_backend.h"
#include "mod_proxy_core_pool.h"
#include "mod_proxy_core_address.h"
#include "mod_proxy_core_spawn.h"
#include "mod_proxy_core.h"

#include "crc32.h"

proxy_backend *proxy_find_backend(server *srv, connection *con, plugin_data *p, buffer *name);

proxy_backend *proxy_backend_init(void) {
	proxy_backend *backend;

	backend = calloc(1, sizeof(*backend));
	backend->pool = proxy_connection_pool_init();
	backend->address_pool = proxy_address_pool_init();
	backend->balancer = PROXY_BALANCE_RR;
	backend->name = buffer_init();
	backend->state = PROXY_BACKEND_STATE_ACTIVE;

	return backend;
}

void proxy_backend_free(proxy_backend *backend) {
	if (!backend) return;

	proxy_connection_pool_free(backend->pool);
	proxy_address_pool_free(backend->address_pool);
	buffer_free(backend->name);

	free(backend);
}

proxy_backends *proxy_backends_init(void) {
	proxy_backends *backends;

	backends = calloc(1, sizeof(*backends));

	return backends;
}

void proxy_backends_free(proxy_backends *backends) {
	if (!backends) return;
	
	ARRAY_STATIC_FREE(backends, proxy_backend, element, proxy_backend_free(element));

	free(backends);
}

void proxy_backends_add(proxy_backends *backends, proxy_backend *backend) {
	ARRAY_STATIC_PREPARE_APPEND(backends);

	backends->ptr[backends->used++] = backend;
}

/**
 * choose an available backend
 *
 */
proxy_backend *proxy_backend_balancer(server *srv, connection *con, proxy_session *sess) {
	size_t i;
	plugin_data *p = sess->p;
	proxy_backends *backends = p->conf.backends;
	unsigned long last_max; /* for the HASH balancer */
	proxy_backend *backend = NULL, *cur_backend = NULL;
	int active_backends = 0, rand_ndx;
	size_t min_used;

	UNUSED(srv);

	/* if we only have one backend just return it. */
	if (backends->used == 1) {
		backend = backends->ptr[0];

		return backend->state == PROXY_BACKEND_STATE_ACTIVE ? backend : NULL;
	}

	/* frist try to select backend based on sticky session. */
	if (sess->sticky_session) {
		/* find backend */
		backend = proxy_find_backend(srv, con, p, sess->sticky_session);
		if (NULL != backend) return backend;
	}

	/* apply balancer algorithm to select backend. */
	switch(p->conf.balancer) {
	case PROXY_BALANCE_CARP:
		/* hash balancing */

		for (i = 0, last_max = ULONG_MAX; i < backends->used; i++) {
			unsigned long cur_max;

			cur_backend = backends->ptr[i];

			if (cur_backend->state != PROXY_BACKEND_STATE_ACTIVE) continue;

			cur_max = generate_crc32c(CONST_BUF_LEN(con->uri.path)) +
				generate_crc32c(CONST_BUF_LEN(cur_backend->name)) + /* we can cache this */
				generate_crc32c(CONST_BUF_LEN(con->uri.authority));
#if 0
			TRACE("hash-election: %s - %s - %s: %ld",
					con->uri.path->ptr,
					cur_backend->name->ptr,
					con->uri.authority->ptr,
					cur_max);
#endif
			if (backend == NULL || (cur_max > last_max)) {
				last_max = cur_max;

				backend = cur_backend;
			}
		}

		break;
	case PROXY_BALANCE_STATIC:
		/* static (only fail-over) */

		for (i = 0; i < backends->used; i++) {
			cur_backend = backends->ptr[i];

			if (cur_backend->state != PROXY_BACKEND_STATE_ACTIVE) continue;

			backend = cur_backend;
			break;
		}

		break;
	case PROXY_BALANCE_SQF:
		/* shortest-queue-first balancing */

		for (i = 0, min_used = SIZE_MAX; i < backends->used; i++) {
			cur_backend = backends->ptr[i];

			if (cur_backend->state != PROXY_BACKEND_STATE_ACTIVE) continue;

			/* the backend is up, use it */
			if (cur_backend->pool->used < min_used ) {
				backend = cur_backend;
				min_used = cur_backend->pool->used;
			}
		}

		break;
	case PROXY_BALANCE_UNSET: /* if not set, use round-robin as default */
	case PROXY_BALANCE_RR:
		/* round robin */

		/**
		 * instead of real RoundRobin we just do a RandomSelect
		 *
		 * it is state-less and has the same distribution
		 */

		active_backends = 0;

		for (i = 0; i < backends->used; i++) {
			cur_backend = backends->ptr[i];

			if (cur_backend->state != PROXY_BACKEND_STATE_ACTIVE) continue;

			active_backends++;
		}

		rand_ndx = (int) (1.0 * active_backends * rand()/(RAND_MAX));

		active_backends = 0;
		for (i = 0; i < backends->used; i++) {
			cur_backend = backends->ptr[i];

			if (cur_backend->state != PROXY_BACKEND_STATE_ACTIVE) continue;

			backend = cur_backend;

			if (rand_ndx == active_backends++) break;
		}

		break;
	}

	return backend;
}

/**
 * choose an available address from the address-pool
 *
 * the backend has different balancers
 */
proxy_address *proxy_address_balancer(server *srv, connection *con, proxy_session *sess) {
	size_t i;
	proxy_backend *backend = sess->proxy_backend;
	proxy_address_pool *address_pool = backend->address_pool;
	unsigned long last_max; /* for the HASH balancer */
	proxy_address *address = NULL, *cur_address = NULL;
	int active_addresses = 0, rand_ndx;
	size_t min_used;

	if (backend->spawn) {
		return proxy_spawn_address_balancer(srv, con, sess, backend->spawn);
	}

	/* if we only have one address just return it. */
	if (address_pool->used == 1) {
		address = address_pool->ptr[0];

		return address->state == PROXY_ADDRESS_STATE_ACTIVE ? address : NULL;
	}

	/* apply balancer algorithm to select address. */
	switch(backend->balancer) {
	case PROXY_BALANCE_CARP:
		/* hash balancing */

		for (i = 0, last_max = ULONG_MAX; i < address_pool->used; i++) {
			unsigned long cur_max;

			cur_address = address_pool->ptr[i];

			if (cur_address->state != PROXY_ADDRESS_STATE_ACTIVE) continue;

			cur_max = generate_crc32c(CONST_BUF_LEN(con->uri.path)) +
				generate_crc32c(CONST_BUF_LEN(cur_address->name)) + /* we can cache this */
				generate_crc32c(CONST_BUF_LEN(con->uri.authority));
#if 0
			TRACE("hash-election: %s - %s - %s: %ld",
					con->uri.path->ptr,
					cur_address->name->ptr,
					con->uri.authority->ptr,
					cur_max);
#endif
			if (address == NULL || (cur_max > last_max)) {
				last_max = cur_max;

				address = cur_address;
			}
		}

		break;
	case PROXY_BALANCE_STATIC:
		/* static (only fail-over) */

		for (i = 0; i < address_pool->used; i++) {
			cur_address = address_pool->ptr[i];

			if (cur_address->state != PROXY_ADDRESS_STATE_ACTIVE) continue;

			address = cur_address;
			break;
		}

		break;
	case PROXY_BALANCE_SQF:
		/* shortest-queue-first balancing */

		for (i = 0, min_used = SIZE_MAX; i < address_pool->used; i++) {
			cur_address = address_pool->ptr[i];

			if (cur_address->state != PROXY_ADDRESS_STATE_ACTIVE) continue;

			/* the address is up, use it */
			if (cur_address->used < min_used ) {
				address = cur_address;
				min_used = cur_address->used;
			}
		}

		break;
	case PROXY_BALANCE_UNSET: /* if not set, use round-robin as default */
	case PROXY_BALANCE_RR:
		/* round robin */

		/**
		 * instead of real RoundRobin we just do a RandomSelect
		 *
		 * it is state-less and has the same distribution
		 */

		active_addresses = 0;

		for (i = 0; i < address_pool->used; i++) {
			cur_address = address_pool->ptr[i];

			if (cur_address->state != PROXY_ADDRESS_STATE_ACTIVE) continue;

			active_addresses++;
		}

		rand_ndx = (int) (1.0 * active_addresses * rand()/(RAND_MAX));

		active_addresses = 0;
		for (i = 0; i < address_pool->used; i++) {
			cur_address = address_pool->ptr[i];

			if (cur_address->state != PROXY_ADDRESS_STATE_ACTIVE) continue;

			address = cur_address;

			if (rand_ndx == active_addresses++) break;
		}

		break;
	}

	return address;
}

void proxy_backend_disable_address(proxy_session *sess, time_t until) {
	proxy_address* address = sess->proxy_con->address;
	proxy_backend* backend = sess->proxy_backend;

	address->state = PROXY_ADDRESS_STATE_DISABLED;
	address->disabled_until = until;

	if (backend->spawn) {
		proxy_spawn_disable_address(sess, until);
	} else {
		backend->disabled_addresses++;
		/* if all addresses in address_pool are disabled, then disable this backend. */
		if (backend->disabled_addresses == backend->address_pool->used) {
			backend->state = PROXY_BACKEND_STATE_DISABLED;
		}
	}
}

void proxy_backend_close_connection(server *srv, proxy_session *sess) {
	if (sess->proxy_backend->spawn) {
		proxy_spawn_session_close(srv, sess);
	} else {
	/* the backend might have been disabled by a full connection pool, re-enable
		* if there is at least one active address.
		*/
		if (sess->proxy_backend->disabled_addresses <= sess->proxy_backend->address_pool->used) {
			sess->proxy_backend->state = PROXY_BACKEND_STATE_ACTIVE;
		}
	}
}
