#ifndef SINO_NETLIB_H
#define SINO_NETLIB_H
#include "socket.h"
#ifdef __cplusplus
extern "C" {
#endif
int DnsResolve(const char *,u32 *);
int HttpGet(u16,const char *,char *,u32);
#ifdef __cplusplus
}
#endif
#endif
