#ifndef SINO_RUN_BRIDGE_H
#define SINO_RUN_BRIDGE_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RUN_BRIDGE_PAYLOAD 5340u
#define RUN_BRIDGE_LEASE 200u /* 2 guest seconds. */
void run_bridge_init(void);
void run_bridge_tick(void);
void run_bridge_abort(void);
u32 run_bridge_request(const u8 *, u32, u32, u8 *, int *);
#ifdef __cplusplus
}
#endif
#endif
