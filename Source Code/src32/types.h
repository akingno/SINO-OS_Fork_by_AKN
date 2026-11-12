#ifndef SINO_TYPES_H
#define SINO_TYPES_H
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int i32;
typedef signed char i8;
/* A physical address. Never dereferenced directly; see mm/addr.h. */
typedef u32 phys_t;
#define PACKED __attribute__((packed))
#ifdef __cplusplus
}
#endif
#endif
