#ifndef SINO_KLOG_H
#define SINO_KLOG_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Always available, including early boot and panic; no automatic newline. */
void klog_write_bytes(const void*, u32);
void pr_debug(const char*);
void pr_debug_hex(u32);
#ifdef __cplusplus
}
#endif
#endif
