#include "../kernel.h"
#include "../api.h"
#include "../klog.h"
#include "page.h"
#include "paging.h"
#include "kmap.h"
#include "test_fault.h"
#include "layout.h"
#include "../lib/kstring.h"
#include "../panic.h"

#define USER_FLAGS (PAGE_PRESENT | PAGE_USER)
#define USER_FIRST_PDE (USER_BASE / TABLE_BYTES)
#define USER_END_PDE ((USER_V2_END + TABLE_BYTES - 1) / TABLE_BYTES)

static_assert(BOOT_POOL_START % TABLE_BYTES == 0 &&
                   BOOT_IDENTITY_BYTES % TABLE_BYTES == 0 &&
                   BOOT_POOL_START < BOOT_IDENTITY_BYTES,
               "initial pool is aligned to whole page tables");
static_assert(1 + (BOOT_IDENTITY_BYTES - BOOT_POOL_START) / TABLE_BYTES <= 16,
               "temporary page tables fit the low boot reserve");
static_assert(KMAP_PDE_INDEX* TABLE_BYTES == KMAP_BASE,
               "KMAP_BASE matches its PDE index");
static_assert(KMAP_PDE_INDEX >= USER_END_PDE,
               "KMAP PDE does not overlap user area");

// 1GiB以下的恒等映射
u32 direct_map_end;
// 内核页目录数组
static u32* kernel_pd;
// 用户进程创建自己的页目录时，要复制内核页目录，于是这里防修改
static u32 kernel_pde_snapshot[TABLE_ENTRIES];
// bool，完成内核分页初始化后，内核 PDE 不再修改
static int kernel_pdes_frozen;

void paging_activate(const u32* pd) {
  write_cr3(virt_to_phys(pd));
}

/*
  初始化一张恒等映射页表，并挂到 pd[index]，不分配任何内存
  pd: 页目录指针
  index: 要修改的PDE索引
  pt: 页表指针
  flags: PTE 使用的权限标志
*/
static void init_identity_table(u32* pd, u32 index, u32* pt, u32 flags) {
  u32 base = index * TABLE_BYTES;  // PTE 起始地址
  // 写 1024 个PTE
  for (u32 i = 0; i < TABLE_ENTRIES; i++) {
    pt[i] = make_entry(base + i * PAGE_SIZE, flags);
  }
  if (!index) {
    pt[0] = 0;  // 第一个4kib不映射
  }
  // 把页表加入页目录
  pd[index] = make_entry(virt_to_phys(pt), PAGE_PRESENT | PAGE_WRITE);
}

/* 临时页目录仅用于开启分页和建立正式页表；paging_kernel_init 已切换 CR3。
 * TODO(MM-11): 审计正式页表中仍需保留的低端映射和启动资源生命周期，
 * 再收紧长期映射及回收不再使用的启动资源；不能直接删除启动过渡映射。
 */
// 在正式的物理内存管理器和内核页表建立之前，用预留内存构造临时页表，让 CPU
// 能够安全开启分页
void paging_bootstrap(void) {
  u32* pd = (u32*)phys_to_virt(BOOT_PD);
  u32* pt = (u32*)phys_to_virt(BOOT_PT);
  memset(pd, 0, PAGE_SIZE);
  init_identity_table(pd, 0, pt, PAGE_PRESENT | PAGE_WRITE);
  for (u32 i = BOOT_POOL_START / TABLE_BYTES;
       i < BOOT_IDENTITY_BYTES / TABLE_BYTES;
       i++) {  // i from 52/4 = 13; i < 64/4; i++
    pt += TABLE_ENTRIES;
    init_identity_table(pd, i, pt, PAGE_PRESENT | PAGE_WRITE);
  }
  // 把页目录给 CPU
  write_cr3(BOOT_PD);
  // 操作 CR0 控制寄存器，开始映射
  write_cr0((read_cr0() & ~(CR0_CD | CR0_NW)) | CR0_PG | CR0_WP);
  pr_debug("PAGING bootstrap CR3=");
  pr_debug_hex(BOOT_PD);
  pr_debug("\n");
}

void paging_kernel_init(void) {
  // dm = direct mapping,不等同于identity mapping
  // dm_pages = ceil(nr_pages / 1024) * 1024
  // 按 1024 页对齐，这样就不用处理最后一个不完整的 4 MiB 区间
  u32 dm_pages = (nr_pages + TABLE_ENTRIES - 1) & ~(TABLE_ENTRIES - 1);
  // 保证直接映射范围至少覆盖启动阶段所需的地址上界
  if (dm_pages < BOOT_IDENTITY_BYTES / PAGE_SIZE) {
    dm_pages = BOOT_IDENTITY_BYTES / PAGE_SIZE;
  }
  // 保证内核直接映射区不会进入从 USER_BASE 开始的用户地址空间，比如截断到 1 GiB
  if (dm_pages > USER_BASE / PAGE_SIZE) {
    dm_pages = USER_BASE / PAGE_SIZE;
  }
  // 上面得到了数量，现在得到实际映射终点
  u32 dm_end = dm_pages * PAGE_SIZE;
  // 页表数量，如 1GiB / 4MiB = 256
  u32 identity_count = dm_end / TABLE_BYTES;

  // page.c 的分配一页
  u32* pd = (u32*)kpage_alloc();
  if (!pd) {
    panic("No memory for kernel page directory", dm_end, free_pages);
  }
  for (u32 i = 0; i < identity_count; i++) {
    // 分配一页
    u32* pt = (u32*)kpage_alloc();
    if (!pt) {
      panic("No memory for kernel page table", i, free_pages);
    }
    // 分配完的一页建立恒等映射
    init_identity_table(pd, i, pt, PAGE_PRESENT | PAGE_WRITE);
    // 在 64 MiB 外，检查是否可用
    if (i >= BOOT_IDENTITY_BYTES / TABLE_BYTES) {
      for (u32 slot = 0; slot < TABLE_ENTRIES; slot++) {
        if (!page_boot_usable(i * TABLE_BYTES +
                              slot * PAGE_SIZE)) {  // 看是否可用
          pt[slot] = 0;
        }
      }
    }
  }
  // 让 BIOS ROM 等区域只读，同时保留 VGA 文本控制台的写入能力。
  u32* pt0 = (u32*)phys_to_virt(entry_address(pd[0]));
  for (u32 pte = 0xA0000 / PAGE_SIZE; pte <= 0xFF000 / PAGE_SIZE; pte++) {
    if (pte != VGA_TEXT_BASE / PAGE_SIZE) {
      pt0[pte] = make_entry(pte * PAGE_SIZE, PAGE_PRESENT);
    }
  }
  // 为 KMAP 创建页表，这里没有上面那个循环
  // KMAP 并不需要在初始化时映射固定的 1024
  // 个物理页。它是为高端物理内存提供临时虚拟映射的区域。
  u32* kmap_pt = (u32*)kpage_alloc();
  if (!kmap_pt) {
    panic("No memory for KMAP page table", KMAP_PDE_INDEX, free_pages);
  }
  pd[KMAP_PDE_INDEX] =
      make_entry(virt_to_phys(kmap_pt), PAGE_PRESENT | PAGE_WRITE);

  // 把pd写进CR3，意味着从启动时的临时页目录切换到刚刚建立的正式内核页目录
  paging_activate(pd);
  // 新分配的 KMAP 页表交给 KMAP 模块初始化
  kmap_init(kmap_pt);

  // 将正式页目录的指针和直接映射区域结束位置保存到 static 变量
  kernel_pd = pd;
  direct_map_end = dm_end;

  // 正式冻结内核页目录结构
  // 并不是禁止所有内核页表修改，而是约束共享内核页目录的结构稳定性
  memcpy(kernel_pde_snapshot, pd, PAGE_SIZE);
  kernel_pdes_frozen = 1;

  pr_debug("PAGING enabled: CR0=");
  pr_debug_hex(read_cr0());
  pr_debug(" CR3=");
  pr_debug_hex(virt_to_phys(pd));
  pr_debug(", null page unmapped\n");
  pr_debug("Direct mapping: [0, ");
  pr_debug_hex(dm_end);
  pr_debug("), KMAP at PDE ");
  pr_debug_hex(KMAP_PDE_INDEX);
  pr_debug("\n");
  pr_debug("VGA text console: 80x25\n");
}

// 通过 paging_kernel_init 创建了内核页目录后，让CPU使用这个内核页目录
void paging_activate_kernel(void) {
  paging_activate(kernel_pd);
}

/* Up to eight process stacks plus boot-time checks; no allocation in guard paths.
 * The kernel is single-core and non-preemptible while these records change. */
enum { KERNEL_GUARDS = 16 };
typedef struct { KernelGuard* owner; u32 id, va, saved; } GuardRecord;
static GuardRecord guards[KERNEL_GUARDS];
static u32 next_guard_id;
int paging_kernel_guard(void* page, KernelGuard* owner) {
  u32 va = (u32)page;
  if (!kernel_pd || !owner || owner->id || !page_aligned(va) || va >= direct_map_end ||
      ((u32)owner < va + PAGE_SIZE &&
       ((u32)owner >= va || va - (u32)owner < sizeof(*owner))))
    return S_INVALID;
  u32 pde = kernel_pd[pde_index(va)];
  if (!(pde & PAGE_PRESENT) || (pde & PAGE_USER)) return S_INVALID;
  u32* pte = &((u32*)phys_to_virt(entry_address(pde)))[pte_index(va)];
  if (!(*pte & PAGE_PRESENT) || (*pte & PAGE_USER) || entry_address(*pte) != va)
    return S_INVALID;
  GuardRecord* record = 0;
  for (u32 i = 0; i < KERNEL_GUARDS; i++) {
    if (guards[i].owner == owner || (guards[i].owner && guards[i].va == va))
      return S_INVALID;
    if (!guards[i].owner && !record) record = &guards[i];
  }
  if (!record || next_guard_id == 0xffffffffu) return S_LIMIT;
  if (!kpage_guard_claim(page)) return S_INVALID;
  u32 id = ++next_guard_id;
  *record = (GuardRecord){owner, id, va, *pte};
  owner->id = id;
  *pte = 0;
  invlpg(va);
  return S_OK;
}
int paging_kernel_unguard(KernelGuard* owner) {
  if (!owner || !owner->id) return S_INVALID;
  for (u32 i = 0; i < KERNEL_GUARDS; i++) {
    GuardRecord* record = &guards[i];
    if (record->owner != owner || record->id != owner->id) continue;
    u32* pt = (u32*)phys_to_virt(entry_address(kernel_pd[pde_index(record->va)]));
    if (pt[pte_index(record->va)]) return S_CHANGED;
    pt[pte_index(record->va)] = record->saved;
    invlpg(record->va);
    kpage_guard_release((void*)record->va);
    owner->id = 0;
    *record = (GuardRecord){0};
    return S_OK;
  }
  return S_INVALID;
}
int paging_is_active(const u32* pd) {
  return pd && page_floor(read_cr3()) == virt_to_phys(pd);
}

/* Page tables are kernel pages, so the direct mapping always reaches them. */
static u32* user_table(const u32* pd, u32 va) {
  if (va < USER_BASE || va >= USER_V2_END) { // 不在范围内，直接返回空指针
    return 0;
  }
  u32 pde = pd[pde_index(va)];
  // 如果页目录项没有允许用户访问的权限，或者根本不存在，函数就返回 0
  if ((pde & USER_FLAGS) != USER_FLAGS) {
    return 0;
  }
  return (u32*)phys_to_virt(entry_address(pde));
}

/* 0x20 对应 Accessed 位，CPU 可能在访问分页结构时修改它。
  0x40 在当前普通 4 KiB 页表结构的 PDE 中并不是有效的 Dirty 位，不过也屏蔽 */
#define PDE_HW_BITS 0x60u
u32* paging_user_create(void) {
  if (kernel_pdes_frozen){ // 每次创建用户页目录时，都检查内核 PDE 是否发生了意外修改
    for (u32 i = 0; i < TABLE_ENTRIES; i++) {
      if (i >= USER_FIRST_PDE && i < USER_END_PDE) {
        continue; //跳过用户地址空间PDE
      }
      if ((kernel_pd[i] & ~PDE_HW_BITS) !=
          (kernel_pde_snapshot[i] & ~PDE_HW_BITS)) {
        panic("paging_user_create: Kernel PDE changed after init", i, kernel_pd[i]);
      }
    }
  }
  for (u32 i = USER_FIRST_PDE; i < USER_END_PDE; i++){
    if (kernel_pd[i] & PAGE_PRESENT){
      return 0;
    }
  }
  u32* pd = (u32*)kpage_alloc();
  if (pd) {
    // 每个新建用户进程的页目录，最初都包含与内核页目录相同的 PDE
    // 未来给进程分配用户内存时，再创建它自己的用户页表
    memcpy(pd, kernel_pd, PAGE_SIZE);
  }
  return pd;
}

/*  建立用户虚拟地址到物理地址映射
    pd: 某个用户进程的页目录
    va: 想要建立映射的用户虚拟地址
    page: 已经准备好的物理页起始页
    write: 是否允许用户写入
*/
int paging_user_map(u32* pd, u32 va, phys_t page, int write) {
  if (!page || va < USER_BASE || va >= USER_V2_END || !page_aligned(va | page)) {
    return 0;
  }
  u32* pt = user_table(pd, va);
  if (!pt) {
    if (pd[pde_index(va)] & PAGE_PRESENT) {
      return 0;
    }
#ifdef SINO_SHELL_BRIDGE
    if (mmtest::fail(pd, mmtest::Stage::Table)) {
      return 0;
    }
#endif
    pt = (u32*)kpage_alloc();
    if (!pt) {
      return 0;
    }
    pd[pde_index(va)] = make_entry(virt_to_phys(pt), USER_FLAGS | PAGE_WRITE);
  } else if ((pt[pte_index(va)] & PAGE_PRESENT) ||
             (write && !(pd[pde_index(va)] & PAGE_WRITE))) {
    return 0;
  }
  pt[pte_index(va)] = make_entry(page, USER_FLAGS | (write ? PAGE_WRITE : 0));
  /* No TLB invalidation: both the new PTE and any new PDE are 0→1
   * transitions, and x86 never caches non-present entries (SDM 4.10.4.3). */
  return 1;
}

// 取消一个用户虚拟页的映射，如果对应页表已经没有任何有效映射则把页表页释放了
phys_t paging_user_unmap(u32* pd, u32 va) {
  if (!page_aligned(va)) {
    return PHYS_NULL;
  }
  u32* pt = user_table(pd, va);
  if (!pt || !(pt[pte_index(va)] & PAGE_PRESENT)) {
    return PHYS_NULL;
  }
  u32 pte = pt[pte_index(va)];
  pt[pte_index(va)] = 0;
  u32 i = 0;
  while (i < TABLE_ENTRIES && !(pt[i] & PAGE_PRESENT)) {
    i++;
  }
  if (i == TABLE_ENTRIES) { // 1024 个 PTE 都没有 PAGE_PRESENT
    pd[pde_index(va)] = 0;
    /* Flush the entire TLB: the paging-structure cache may still reference
     * this page table, so a single invlpg is insufficient.  Must complete
     * before the table page is freed.  If CR4.PGE is ever enabled, the
     * no-global-pages invariant that exempts inactive PDs must be revisited. */
    if (paging_is_active(pd)) {
      paging_activate(pd);
    }
    kpage_free(pt);
  } else {
    /* Only the data PTE changed (1→0); invlpg suffices.  Inactive PDs need
     * no invalidation: every CR3 switch flushes the TLB (no global pages). */
    if (paging_is_active(pd)) {
      invlpg(va);
    }
  }
  return entry_address(pte);
}

// 查询某个用户虚拟地址实际生效的分页权限标志
u32 paging_user_flags(const u32* pd, u32 va) {
  const u32* pt = user_table(pd, va);
  return pt ? pt[pte_index(va)] & pd[pde_index(va)] & (USER_FLAGS | PAGE_WRITE)
            : 0;
}

// 统计一个进程当前有多少个有效的用户虚拟页映射
u32 paging_user_count(const u32* pd) {
  u32 count = 0;
  for (u32 pde = USER_FIRST_PDE; pde < USER_END_PDE; pde++) {
    const u32* pt = user_table(pd, pde * TABLE_BYTES);
    if (!pt) {
      continue;
    }
    for (u32 i = 0; i < TABLE_ENTRIES; i++){
      if (pt[i] & PAGE_PRESENT){
        count++;
      }
    }
  }
  return count;
}

// 寻找一段尚未映射的连续虚拟地址范围
u32 paging_user_find_gap(const u32* pd, u32 start, u32 end, u32 bytes) {
  if (start < USER_DYNAMIC_BASE || end > USER_DYNAMIC_END || start >= end ||
      !bytes || bytes > end - start || !page_aligned(start | end | bytes)) {
    return 0;
  }
  u32 gap = start;
  for (u32 va = start; va < end;) {
    const u32* pt = user_table(pd, va);
    if (pt) {
      if (pt[pte_index(va)] & PAGE_PRESENT)
        gap = va + PAGE_SIZE;
      va += PAGE_SIZE;
    } else {
      u32 next = (pde_index(va) + 1) * TABLE_BYTES;
      va = next < end ? next : end;
    }
    if (va - gap >= bytes) {
      return gap;
    }
  }
  return 0;
}

// 彻底销毁一个用户进程的分页结构，同时释放仍由这些映射拥有的数据物理页。
// paging_user_unmap 只解除一个页面的映射，这个会释放整个进程的用户页表
// TODO: 如果引入COW,这里逻辑就要改
void paging_user_destroy(u32* pd) {
  if (paging_is_active(pd)){
    panic("paging_user_destroy: Destroying active user page directory", virt_to_phys(pd), 0);
  }
  for (u32 pde = USER_FIRST_PDE; pde < USER_END_PDE; pde++) {
    u32* pt = user_table(pd, pde * TABLE_BYTES);
    if (!pt) {
      continue;
    }
    pd[pde] = 0;
    for (u32 i = 0; i < TABLE_ENTRIES; i++) {
      if (pt[i] & PAGE_PRESENT) {
        page_free(entry_address(pt[i]));
      }
    }
    kpage_free(pt);
  }
  kpage_free(pd);
}

void paging_protect_selftest(void) {
  u32 saved_free = free_pages;
  void* run = kpage_alloc_run(2);
  if (!run)
    panic("paging_protect_selftest: alloc", 0, free_pages);
  KernelGuard guard = {}, other = {};
  if (paging_kernel_guard((void*)VGA_TEXT_BASE, &guard) != S_INVALID ||
      paging_kernel_guard((u8*)run + 1, &guard) != S_INVALID ||
      paging_kernel_guard(run, &guard) != S_OK ||
      paging_kernel_guard(run, &other) != S_INVALID ||
      paging_kernel_guard((u8*)run + PAGE_SIZE, &guard) != S_INVALID ||
      kpage_free_run(run, 2) || page_free((phys_t)run))
    panic("Guard ownership self-check", 0, 0);
  KernelGuard copied = guard;
  if (paging_kernel_unguard(&copied) != S_INVALID)
    panic("Copied guard token accepted", 0, 0);

  u32 fc = fault_count;
  __asm__ volatile(
      "movl $1f, %[res]\n\t"
      "movl $14, %[exp]\n\t"
      "movl $0xDEAD, (%[a])\n\t"
      "1:"
      : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
      : [a] "r"(run)
      : "memory");
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc + 1)
    panic("paging_protect_selftest: guard", fault_count, fc + 1);

  if (paging_kernel_unguard(&guard) != S_OK ||
      paging_kernel_unguard(&guard) != S_INVALID)
    panic("Guard restore self-check", 0, 0);
  fc = fault_count;
  __asm__ volatile(
      "movl $1f, %[res]\n\t"
      "movl $14, %[exp]\n\t"
      "movl $0xBEEF, (%[a])\n\t"
      "1:"
      : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
      : [a] "r"(run)
      : "memory");
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc)
    panic("paging_protect_selftest: unprotect", fault_count, fc);
  u32 va = (u32)run;
  u32* pt = (u32*)phys_to_virt(entry_address(kernel_pd[pde_index(va)]));
  pt[pte_index(va)] &= ~PAGE_WRITE;
  invlpg(va);
  u32 original = pt[pte_index(va)];
  if (paging_kernel_guard(run, &guard) != S_OK ||
      paging_kernel_unguard(&guard) != S_OK || pt[pte_index(va)] != original)
    panic("Guard lost original PTE", va, original);
  pt[pte_index(va)] |= PAGE_WRITE;
  invlpg(va);
  if (!kpage_free_run(run, 2)) panic("Guard allocation free", va, 0);
  if (free_pages != saved_free)
    panic("paging_protect_selftest: leak", free_pages, saved_free);

  fc = fault_count;
  __asm__ volatile(
      "movl $1f, %[res]\n\t"
      "movl $14, %[exp]\n\t"
      "movl $0xDEAD, (%[a])\n\t"
      "1:"
      : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
      : [a] "r"((void*)0xF0000)
      : "memory");
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc + 1)
    panic("paging_protect_selftest: ROM", fault_count, fc + 1);

  volatile u32* vga = (volatile u32*)VGA_TEXT_BASE;
  u32 val = *vga;
  fc = fault_count;
  __asm__ volatile(
      "movl $1f, %[res]\n\t"
      "movl $14, %[exp]\n\t"
      "movl %[v], (%[a])\n\t"
      "1:"
      : [res] "=m"(fault_resume), [exp] "=m"(fault_expected)
      : [a] "r"(vga), [v] "r"(val)
      : "memory");
  fault_expected = 0;
  fault_resume = 0;
  if (fault_count != fc)
    panic("paging_protect_selftest: VGA", fault_count, fc);

  pr_debug("PAGING protection self-check passed\n");
}
