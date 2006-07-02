#ifndef WIN32_SOCKET_H
#define WIN32_SOCKET_H

#ifdef _WIN32

#include <winsock2.h>

#define ECONNRESET WSAECONNRESET
#define EINPROGRESS WSAEINPROGRESS
#define EALREADY WSAEALREADY
#define ENOTCONN WSAENOTCONN
#define ioctl ioctlsocket
#define hstrerror(x) ""
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define ssize_t int

int inet_aton(const char *cp, struct in_addr *inp);
#define HAVE_INET_ADDR
#undef HAVE_INET_ATON

#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include <netdb.h>
#endif

#endif
