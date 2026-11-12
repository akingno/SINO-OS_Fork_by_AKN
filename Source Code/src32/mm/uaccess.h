#ifndef SINO_MM_UACCESS_H
#define SINO_MM_UACCESS_H

#include "../types.h"

// USER_READ 需要 PRESENT + USER
// USER_WRITE 需要 PRESENT + USER + WRITE
typedef enum { USER_READ, USER_WRITE } UserAccess;

/* Returns S_OK or an S_* error. A prepared range may be borrowed only
 * during the current syscall, while its address space stays active. */
int uaccess_prepare(u32* pd, u32 address, u32 bytes, UserAccess access);
int copy_from_user(u32* pd, void* out, u32 address, u32 bytes);
/* Fixed-size copies leave the destination unchanged on prepare failure.
 * String copies may leave a prefix on failure. See docs/mm-access.md. */
int copy_to_user(u32* pd, u32 address, const void* source, u32 bytes);
int copy_string_from_user(u32* pd, char* out, u32 address, u32 capacity);

#define UACCESS_TRY(call) do {            \
  int uaccess_status = (call);           \
  if (uaccess_status != 0)               \
    return uaccess_status;               \
} while (0)

#endif
