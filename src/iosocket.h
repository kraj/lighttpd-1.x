#ifndef _IOSOCKET_H_
#define _IOSOCKET_H_

#if defined HAVE_LIBSSL && defined HAVE_OPENSSL_SSL_H
# define USE_OPENSSL
# include <openssl/ssl.h>
#endif

typedef enum {
	IOSOCKET_TYPE_UNSET,
	IOSOCKET_TYPE_SOCKET,
	IOSOCKET_TYPE_PIPE
} iosocket_t;

/**
 * a non-blocking fd
 */
typedef struct {
	int fd;
	int fde_ndx;

#ifdef USE_OPENSSL
	SSL *ssl;
#endif

	iosocket_t type; /**< sendfile on solaris doesn't work on pipes */
} iosocket;

iosocket *iosocket_init(void);
void iosocket_free(iosocket *sock);

#endif
