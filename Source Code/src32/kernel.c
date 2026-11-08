#include "klog.h"
#include "rtc.h"
#include "kernel.h"
#include "mm/page.h"
#include "mm/paging.h"
#include "storage.h"
#include "disk.h"
#include "process.h"
#include "vfs.h"
#include "net.h"
#include "keyboard.h"
#include "fs/boot_fat12.h"
#include "panic.h"

#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

volatile u32 ticks, keyboard_irqs, mouse_irqs, mouse_packets;
volatile i32 mouse_x = 512, mouse_y = 384;
volatile u8 mouse_buttons, mouse_dirty;
int mouse_present;
int mouse_absolute;
u32 fault_resume, fault_expected, fault_count;
static volatile u8 keys[128];
static volatile u32 key_head, key_tail;
static volatile MouseEvent mouse_events[128];
static volatile u32 mouse_head, mouse_tail;
volatile u32 mouse_overflows;
static u8 packet[3], packet_pos;
typedef struct PACKED {
  u16 low, selector;
  u8 zero, flags;
  u16 high;
} Gate;
typedef struct PACKED {
  u16 limit;
  u32 base;
} TablePointer;
static Gate idt[256];
extern void* isr_table[256];
static void wait_input(void) {
  for (u32 n = 0; n < 100000; n++)
    if (!(inb(0x64) & 2))
      return;
}
static void controller(u8 v) {
  wait_input();
  outb(0x64, v);
}
static void data(u8 v) {
  wait_input();
  outb(0x60, v);
}
static int device_command(int mouse, u8 cmd) {
  if (mouse)
    controller(0xd4);
  data(cmd);
  for (u32 n = 0; n < 200000; n++) {
    u8 s = inb(0x64);
    if (s & 1) {
      u8 v = inb(0x60);
      if (!!(s & 0x20) == mouse)
        return v == 0xfa;
    }
  }
  return 0;
}
static void mouse_emit(i32 x, i32 y, u8 buttons, i8 wheel) {
  u8 changed = mouse_buttons ^ buttons;
  mouse_buttons = buttons;
  mouse_x = x < 0 ? 0 : x >= SCREEN_WIDTH ? SCREEN_WIDTH - 1 : x;
  mouse_y = y < 0 ? 0 : y >= SCREEN_HEIGHT ? SCREEN_HEIGHT - 1 : y;
  u32 previous = (mouse_head - 1) & 127, next = (mouse_head + 1) & 127;
  if (!changed && !wheel && mouse_head != mouse_tail &&
      !mouse_events[previous].changed && !mouse_events[previous].wheel &&
      mouse_events[previous].buttons == buttons) {
    mouse_events[previous].x = mouse_x;
    mouse_events[previous].y = mouse_y;
    mouse_events[previous].time = ticks;
  } else {
    if (next == mouse_tail) {
      mouse_tail = (mouse_tail + 1) & 127;
      mouse_overflows++;
    }
    mouse_events[mouse_head] =
        (MouseEvent){mouse_x, mouse_y, ticks, buttons, changed, wheel};
    mouse_head = next;
  }
  mouse_dirty = 1;
}
typedef struct {
  u32 a, b, c, d;
} VmReply;
static VmReply vm_call(u32 command, u32 argument) {
  u32 a = 0x564d5868, b = argument, c = command, d = 0x5658;
  __asm__ volatile("inl %%dx,%%eax"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d)::"memory");
  return (VmReply){a, b, c, d};
}
static void vm_mouse_init(void) {
  VmReply r = vm_call(10, 0);
  if (r.b != 0x564d5868)
    return;
  (void)vm_call(41, 0x45414552);
  r = vm_call(40, 0);
  if ((r.a >> 16) || !(r.a & 65535))
    return;
  r = vm_call(39, 1);
  if (r.a != 0x3442554a)
    return;
  (void)vm_call(41, 0x53424152);
  mouse_absolute = 1;
  pr_debug("VMMOUSE absolute pointer ready\n");
}
void mouse_poll(void) {
  if (!mouse_absolute)
    return;
  u32 flags;
  __asm__ volatile("pushfl; popl %0" : "=r"(flags));
  cli();
  for (int i = 0; i < 64; i++) {
    VmReply r = vm_call(40, 0);
    if ((r.a >> 16) || (r.a & 65535) < 4)
      break;
    r = vm_call(39, 4);
    u8 buttons =
        (r.a & 0x20 ? 1 : 0) | (r.a & 0x10 ? 2 : 0) | (r.a & 8 ? 4 : 0);
    mouse_packets++;
    mouse_emit((i32)((r.b & 65535) * (SCREEN_WIDTH - 1) / 65535),
               (i32)((r.c & 65535) * (SCREEN_HEIGHT - 1) / 65535), buttons,
               (i8)r.d);
  }
  if (flags & 0x200)
    sti();
}
static void mouse_byte(u8 v) {
  if (!packet_pos && !(v & 8))
    return;
  packet[packet_pos++] = v;
  if (packet_pos != 3)
    return;
  packet_pos = 0;
  mouse_packets++;
  i32 x = mouse_x, y = mouse_y;
  if (!(packet[0] & 0xc0)) {
    x += packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    y -= packet[2] - ((packet[0] & 0x20) ? 256 : 0);
  }
  /* Coalesce motion only. Preserve button edges even during a slow repaint. */
  mouse_emit(x, y, packet[0] & 7, 0);
}
int mouse_read(MouseEvent* event) {
  u32 flags;
  __asm__ volatile("pushfl; popl %0" : "=r"(flags));
  cli();
  int ready = mouse_head != mouse_tail;
  if (ready) {
    *event = mouse_events[mouse_tail];
    mouse_tail = (mouse_tail + 1) & 127;
  }
  mouse_dirty = mouse_head != mouse_tail;
  if (flags & 0x200)
    sti();
  return ready;
}
void irq_dispatch(Frame* f) {
  if (f->vector < 32 || f->vector == 0x80)
    if (process_interrupt(f))
      return;
  if (f->vector < 32) {
    if (f->vector == 3) {
      pr_debug("BREAKPOINT exception handled\n");
      return;
    }
    if (fault_expected == f->vector && fault_resume) {
      fault_count++;
      f->eip = fault_resume;
      fault_expected = 0;
      fault_resume = 0;
      pr_debug("EXPECTED exception handled: ");
      pr_debug_hex(f->vector);
      pr_debug("\n");
      return;
    }
    pr_debug("Fault EIP=");
    pr_debug_hex(f->eip);
    pr_debug(" CR2=");
    u32 cr2;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    pr_debug_hex(cr2);
    pr_debug("\n");
    panic("CPU exception", f->vector, f->error);
  }
  if (f->vector == 0x80) {
    if (f->eax == 2) {
      f->eax = 32;
      return;
    }
#if defined(SINO_SHELL_BRIDGE) || defined(SELFTEST)
    extern u32 native_base, native_size;
    extern int native_running;
    if (!native_running) {
      f->eax = 0xffffffffu;
      return;
    }
    if (f->eax == 0) {
      f->eip = (u32)native_exit;
      return;
    }
    if (f->eax == 1 && f->ebx >= native_base && f->edx <= native_size &&
        f->ebx - native_base <= native_size - f->edx) {
      native_write((const char*)f->ebx, f->edx);
      f->eax = f->edx;
      return;
    }
#endif
    f->eax = 0xffffffffu;
    return;
  }
  if (f->vector >= 32 && f->vector < 48) {
    u32 irq = f->vector - 32;
    if (irq == 0) {
      ticks++;
      process_interrupt(f);
    }
#ifdef SINO_SHELL_BRIDGE
    if (irq == 4) shell_bridge_irq();
#endif
    if (irq == 1 || irq == 12) {
      u8 s = inb(0x64);
      if (s & 1) {
        u8 v = inb(0x60);
        if (s & 0x20) {
          mouse_irqs++;
          if (mouse_absolute)
            mouse_poll();
          else
            mouse_byte(v);
        } else {
          keyboard_irqs++;
          u32 next = (key_head + 1) & 127;
          if (next != key_tail) {
            keys[key_head] = v;
            key_head = next;
          }
        }
      }
    }
    if (irq >= 8)
      outb(0xa0, 0x20);
    outb(0x20, 0x20);
    return;
  }
  panic("Unexpected interrupt", f->vector, f->eip);
}
void arch_init(void) {
  cli();
  outb(0x21, 0xff);
  outb(0xa1, 0xff);
  for (u32 i = 0; i < 256; i++) {
    u32 a = (u32)isr_table[i];
    idt[i] = (Gate){(u16)a, 8, 0, 0x8e, (u16)(a >> 16)};
  }
  idt[0x80].flags = 0xee;
  TablePointer p = {sizeof(idt) - 1, (u32)idt};
  __asm__ volatile("lidt %0" ::"m"(p));
  outb(0x20, 0x11);
  outb(0x80, 0);
  outb(0xa0, 0x11);
  outb(0x80, 0);
  outb(0x21, 0x20);
  outb(0x80, 0);
  outb(0xa1, 0x28);
  outb(0x80, 0);
  outb(0x21, 4);
  outb(0xa1, 2);
  outb(0x21, 1);
  outb(0xa1, 1);
  outb(0x21, 0xff);
  outb(0xa1, 0xff);
  outb(0x43, 0x36);
  outb(0x40, (u8)(1193182 / 100));
  outb(0x40, (u8)((1193182 / 100) >> 8));
  controller(0xad);
  controller(0xa7);
  for (u32 i = 0; i < 64 && (inb(0x64) & 1); i++)
    (void)inb(0x60);
  controller(0x60);
  data(0x43);
  controller(0xae);
  controller(0xa8);
  if (!device_command(0, 0xf4))
    pr_debug("Keyboard ACK unavailable; continuing\n");
  mouse_present = device_command(1, 0xf6) && device_command(1, 0xf4);
  outb(0x21, 0xf8);
  outb(0xa1, mouse_present ? 0xef : 0xff);
  pr_debug("IDT 256 gates, PIC remapped, PIT 100 Hz\n");
  pr_debug(mouse_present ? "32-bit PS/2 mouse IRQ12 ready\n"
                         : "PS/2 mouse unavailable\n");
  if (mouse_present)
    vm_mouse_init();
}
int key_read(void) {
  while (key_head != key_tail) {
    u8 v = keys[key_tail];
    key_tail = (key_tail + 1) & 127;
    int key = keyboard_decode(v);
    if (key)
      return key;
  }
  return 0;
}
void ps2_byte(int aux, u8 value) {
  controller(aux ? 0xd3 : 0xd2);
  data(value);
  for (u32 n = 0; n < 200000 && (inb(0x64) & 1); n++)
    __asm__ volatile("nop");
}

static u32 boot_framebuffer(void) {
  const u8* vbe = (const u8*)0x5800;
  u32 fb = *(const u32*)(vbe + 40);
  if (!fb || *(const u16*)(vbe + 18) != SCREEN_WIDTH ||
      *(const u16*)(vbe + 20) != SCREEN_HEIGHT || vbe[25] != 32 || vbe[27] != 6 ||
      vbe[31] != 8 || vbe[32] != 16 || vbe[33] != 8 || vbe[34] != 8 ||
      vbe[35] != 8 || vbe[36] != 0)
    panic("VBE direct-color mode unavailable", fb, vbe[25]);
  return fb;
}
void kernel_tests(void) {
  page_allocator_test();
  __asm__ volatile("int3");
  u32 v = 2;
  __asm__ volatile("int $0x80" : "+a"(v)::"memory");
  if (v != 32)
    panic("INT80 ABI", v, 32);
#ifdef SELFTEST
  test_invalid_opcode();
  test_page_fault();
  if (fault_count != 2)
    panic("Exception recovery", fault_count, 2);
  pr_debug("FAULT recovery self-check passed\n");
#endif
}
void kmain(void) {
  pr_debug("SINO 0.10 native 32-bit kernel entered, CS=0x08\n");
#ifdef SINO_BUILD_ID
  pr_debug("SINO BUILD ID: " SINO_BUILD_ID "\n");
#endif
  arch_init();
  paging_bootstrap();
  page_init();
  u32 identity_bytes = (usable_memory + 0x3fffffu) & ~0x3fffffu;
  if (identity_bytes < BOOT_IDENTITY_BYTES)
    identity_bytes = BOOT_IDENTITY_BYTES;
  paging_kernel_init(identity_bytes, boot_framebuffer());
  page_publish_extended();
  if (!fs_init())
    panic("FAT12 mount failed", 0, 0);
  process_init();
  net_init();
  kernel_tests();
  net_tests();
  vfs_init();
#if defined(SINO_SHELL_BRIDGE) || defined(SELFTEST)
  shell_command_init();
#endif
  disk_init();
  storage_init();
  rtc_init();
  sti();
  u32 begin = ticks;
  while (ticks - begin < 3)
    halt();
  pr_debug("TIMER IRQ0 delivered in protected mode\n");
#ifdef SELFTEST
  shell_tests();
#endif
#ifdef SINO_SHELL_BRIDGE
  shell_bridge_init();
#endif
  pr_debug("SINO CLI ready\n");
  /* 【简化实现：系统推进】RTC、ShellBridge、网络和用户调度串行推进，循环尾统一 halt；
   * 就绪用户线程也需等主循环再次进入 process_step，延迟受其他工作影响。
   * 替换方向：将调度/空闲决策独立出来，有可运行工作时继续调度，只有无工作
   * 时休眠；后台处理需配套任务化，不能只删除 halt 造成忙等。 */
  for (;;) {
    rtc_poll();
#ifdef SINO_SHELL_BRIDGE
    shell_bridge_poll();
#endif
    net_poll();
    process_step();
    cli();
    sti();
    halt();
  }
}
