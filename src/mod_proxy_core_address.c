#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "sys-socket.h"
#include "mod_proxy_core_address.h"

proxy_address *proxy_address_init(void) {
	proxy_address *address;

	address = calloc(1, sizeof(*address));

	address->name = buffer_init();

	return address;
}

void proxy_address_free(proxy_address *address) {
	if (!address) return;

	buffer_free(address->name);

	free(address);
}


proxy_address_pool *proxy_address_pool_init(void) {
	proxy_address_pool *address_pool;

	address_pool = calloc(1, sizeof(*address_pool));

	return address_pool;
}

void proxy_address_pool_free(proxy_address_pool *address_pool) {
	if (!address_pool) return;

	FOREACH(address_pool, element, proxy_address_free(element))

	free(address_pool);
}

void proxy_address_pool_add(proxy_address_pool *address_pool, proxy_address *address) {
	ARRAY_STATIC_PREPARE_APPEND(address_pool);
	
	address_pool->ptr[address_pool->used++] = address;
}

int  proxy_address_pool_add_string(proxy_address_pool *address_pool, buffer *name) {
	struct addrinfo *res = NULL, pref, *cur;
	int ret;

	pref.ai_flags = 0;
	pref.ai_family = PF_UNSPEC;
	pref.ai_socktype = SOCK_STREAM;
	pref.ai_protocol = 0;
	pref.ai_addrlen = 0;
	pref.ai_addr = NULL;
	pref.ai_canonname = NULL;
	pref.ai_next = NULL;

	if (0 != (ret = getaddrinfo(name->ptr, "80", &pref, &res))) {
		ERROR("getaddrinfo failed: %s", gai_strerror(ret));

		return -1;
	}

	for (cur = res; cur; cur = cur->ai_next) {
		proxy_address *a = proxy_address_init();

		memcpy(&(a->addr), cur->ai_addr, cur->ai_addrlen);

		a->state = PROXY_ADDRESS_STATE_ACTIVE;

		buffer_copy_string(a->name, inet_ntoa(a->addr.ipv4.sin_addr));

		proxy_address_pool_add(address_pool, a);
	}

	freeaddrinfo(res);

	return 0;
}


