#ifndef SINO_MM_UACCESS_H
#define SINO_MM_UACCESS_H

#include "../types.h"

typedef enum { USER_READ, USER_WRITE } UserAccess;

/* Returns S_OK or an S_* error. A prepared range may be borrowed only
 * during the current syscall, while its address space stays active. */
int uaccess_prepare(u32* pd, u32 address, u32 bytes, UserAccess access);
int uaccess_copy_in(u32* pd, void* out, u32 address, u32 bytes);
int uaccess_copy_string(u32* pd, char* out, u32 address, u32 capacity);

#define UACCESS_TRY(call) do {            \
  int uaccess_status = (call);           \
  if (uaccess_status != 0)               \
    return uaccess_status;               \
} while (0)

#endif
