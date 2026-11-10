#ifndef SINO_KERNEL_H
#define SINO_KERNEL_H
#include "types.h"
#include "x86.h"
typedef struct {
  u32 edi, esi, ebp, esp, ebx, edx, ecx, eax, vector, error, eip, cs, eflags;
} Frame;
extern volatile u32 ticks, keyboard_irqs, mouse_irqs, mouse_packets;
extern volatile i32 mouse_x, mouse_y;
extern volatile u8 mouse_buttons, mouse_dirty;
extern u32 fault_resume, fault_expected, fault_count;
extern int mouse_present;
void arch_init(void);
int key_read(void);
typedef struct {
  i32 x, y;
  u32 time;
  u8 buttons, changed;
  i8 wheel;
} MouseEvent;
int mouse_read(MouseEvent*);
void mouse_poll(void);
extern int mouse_absolute;
extern volatile u32 mouse_overflows;
void ps2_byte(int, u8);
void native_write(const char*, u32);
void run_native(u32);
void native_exit(void);
void test_page_fault(void);
void test_invalid_opcode(void);
void shell_command_init(void);
void shell_bridge_init(void);
void shell_bridge_poll(void);
void shell_bridge_irq(void);
void kernel_tests(void);
void shell_tests(void);
#endif
