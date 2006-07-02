#ifndef _SYS_PROCESS_H_
#define _SYS_PROCESS_H_

#ifdef _WIN32
#include <process.h>
#define pid_t int
#else
#include <unistd.h>
#endif

#endif