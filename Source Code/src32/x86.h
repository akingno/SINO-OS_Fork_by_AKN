#ifndef SINO_X86_H
#define SINO_X86_H
/* Wrappers for the privileged x86 instructions the kernel uses. */
#include "types.h"

static inline void outb(u16 p, u8 v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline u8 inb(u16 p) {
  u8 v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline void outw(u16 p, u16 v) {
  __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p));
}
static inline u16 inw(u16 p) {
  u16 v;
  __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline void cli(void) {
  __asm__ volatile("cli" ::: "memory");
}
static inline void sti(void) {
  __asm__ volatile("sti" ::: "memory");
}
static inline void halt(void) {
  __asm__ volatile("hlt");
}

#define CR0_WP (1u << 16) /* Supervisor writes also honour read-only pages. */
#define CR0_NW (1u << 29)
#define CR0_CD (1u << 30)
#define CR0_PG (1u << 31)

static inline u32 read_cr0(void) {
  u32 value;
  __asm__ volatile("mov %%cr0,%0" : "=r"(value));
  return value;
}
static inline void write_cr0(u32 value) {
  __asm__ volatile("mov %0,%%cr0" ::"r"(value) : "memory");
}
/* The linear address that caused the most recent page fault. */
static inline u32 read_cr2(void) {
  u32 value;
  __asm__ volatile("mov %%cr2,%0" : "=r"(value));
  return value;
}
/* CR3 holds the physical address of the active page directory; loading it
 * flushes the TLB. */
static inline phys_t read_cr3(void) {
  u32 value;
  __asm__ volatile("mov %%cr3,%0" : "=r"(value)::"memory");
  return value;
}
static inline void write_cr3(phys_t directory) {
  __asm__ volatile("mov %0,%%cr3" ::"r"(directory) : "memory");
}
static inline void invlpg(u32 address) {
  __asm__ volatile("invlpg (%0)" ::"r"(address) : "memory");
}
#endif
