#include "internal.h"
#include "../klog.h"
#include "../net.h"
#include "../panic.h"
#include "../sync/irq.h"
#include "../lib/kstring.h"
#include "../mm/page.h"

extern "C" {
  task::ScheduleStats task_schedule_stats;
}

namespace task {
  namespace {
    u32 quantum_end;
    phys_t kernelDirectory;
    PageWatermark observedWatermark;
    bool reclaimPending;

    void requireUnbound(bool interrupts) {
      if (current || currentThread != -1 || read_cr3() != kernelDirectory ||
          !!(sync::readState() & sync::InterruptEnabled) != interrupts) {
        panic("Scheduler context/CR3/IF contract", read_cr3(), (u32)currentThread);
      }
    }

    void observeWatermark() {
      const auto watermark = page_watermark();
      if (watermark != observedWatermark) {
        pr_debug("MEM watermark from=");
        pr_debug_hex((u32)observedWatermark);
        pr_debug(" to=");
        pr_debug_hex((u32)watermark);
        pr_debug(" free=");
        pr_debug_hex(free_pages);
        pr_debug("\n");
        observedWatermark = watermark;
        reclaimPending = watermark != PageWatermark::Normal;
      }
    }

    void containFault(u32 vector) {
      process_faults++;
      current->retire(PROCESS_FAULTED, -(int)vector, vector);
      pr_debug("USER fault contained pid=");
      pr_debug_hex(current->pid);
      pr_debug(" vector=");
      pr_debug_hex(vector);
      pr_debug("\n");
    }

    void logAnonOom(UserPageStatus status, u32 address) {
      if (status == UserPageStatus::Invalid) { return; }
      pr_debug(status == UserPageStatus::TableNoMemory ? "USER anonymous page table OOM address="
                                                       : "USER anonymous data page OOM address=");
      pr_debug_hex(address);
      pr_debug("\n");
    }

    bool retryAnonFault(Thread& thread) {
      const u32 page = thread.anonFaultPage;
      thread.anonFaultPage = 0;
      u32* pd = current->pd;
      const bool write = (thread.saved.frame.error & 2u) != 0;
      if (anonPage(pd, page, 0, write) == UserPageStatus::Ok) {
        const u32 mapped = paging_user_flags(pd, page);
        const u32 needed = PAGE_PRESENT | PAGE_USER | (write ? PAGE_WRITE : 0);
        if ((mapped & needed) == needed) { return true; }
        const auto status = anonPage(pd, page, 1, write);
        if (status == UserPageStatus::Ok) { return true; }
        logAnonOom(status, page);
      }
      containFault(14);
      return false;
    }
  }

  void requestReclaim() { reclaimPending = true; }
  bool hasWork(u32 now) { return reclaimPending || queuedWork(now); }

  void initScheduler() {
    kernelDirectory = read_cr3();
    Process& p = processes[0];
    Thread& t = p.thread[0];
    const u32 now = ticks;
    if (hasWork(now)) { panic("Scheduler initial work", 0, 0); }
    p.state = 1;
    t.process = &p;
    t.state = ThreadState::RUNNING;
    t.sleep(32);
    if (hasWork(now) || !hasWork(now + 32)) {
      panic("Scheduler sleep deadline", 0, 0);
    }
    expireTimers(now + 32);
    if (takeReady() != &t || t.state != ThreadState::RUNNING) {
      panic("Scheduler sleep wake", 0, 0);
    }
    t.cancel();
    armTimer(p.timer, now + 64, TimerKind::Run, &p);
    expireTimers(now + 64);
    if (takeReap() != &p || p.state != 2 || hasWork(now + 64)) {
      panic("Scheduler run/reap queue", 0, 0);
    }
    memset((void*)&p, 0, sizeof(p));
    waitSelfTest();
    pr_debug("TASK scheduler state self-check passed\n");
  }

  void continued() {
    requireUnbound(false);
    task_schedule_stats.continued++;
  }

  void beforeIdle() {
    requireUnbound(false);
    // 在实际 HLT 边界独立记录违规，避免只观察宿主耗时来验证空闲策略。
    if (hasWork(ticks)) {
      task_schedule_stats.idleWithWork++;
      panic("Scheduler idle with work", ticks, task_schedule_stats.idleWithWork);
    }
    task_schedule_stats.idleHalts++;
  }
  /* 【简化实现：抢占边界】仅处理 Ring 3 的切出，用户态到期可被抢占，内核态
  * 系统调用必须执行完或显式挂起后才调度；长磁盘操作仍会拖延其他进程。
  * 替换方向：先限制内核长路径并引入可阻塞 I/O；若进一步支持内核抢占，
  * 必须配套线程内核上下文、临界区和共享状态同步，不能直接放开中断切换。 */
  int interrupt(Frame* f) {
    if ((f->cs & 3) != 3) {
      return 0;
    }
    if (!current) {
      panic("Orphan user frame", f->eip, 0);
    }
    if (f->vector < 32) {
      if (f->vector == 14 && (f->error & ~2u) == 4u) {
        u32 address = read_cr2();
        const u32 page = page_floor(address);
        const auto status = anonPage(current->pd, page, 1, (f->error & 2u) != 0);
        if (status == UserPageStatus::Ok) {
          return 1;
        }
        logAnonOom(status, address);
        if (status == UserPageStatus::ReclaimDataNoMemory) {
          current->thread[currentThread].anonFaultPage = page;
          leave(f);
          return 1;
        }
      }
      containFault(f->vector);
      leave(f);
      return 1;
    }
    if (f->vector == 0x80) {
      if (current->caps & CAP_NETWORK) {
        net_poll();
      }
      int r = dispatchSyscall(f);
      if (current->state == 2 && !user_switch_out) { leave(f); }
      f->eax = (u32)r;
      if (user_switch_out) {
        Thread* t = &current->thread[currentThread];
        /* A parked syscall keeps its number for the retry. */
        if (t->state != ThreadState::BLOCKED) {
          t->saved.frame.eax = (u32)r;
        }
      }
      return 1;
    }
    if (f->vector == 32 && (i32)(ticks - quantum_end) >= 0) {
      leave(f);
    }
    return 0;
  }
  // FIFO 就绪队列；候选预算取本轮队列快照，重试失败后不会在本轮无限重访。
  StepResult step() {
    requireUnbound(true);
    task_schedule_stats.passes++;
    reapAndExpireRuns();
    observeWatermark();
    if (reclaimPending) {
      reclaimPending = false;
      const auto watermark = page_watermark();
      if (watermark != PageWatermark::Normal) {
        reclaimPending = purgeOneRegion();
        if (!reclaimPending && watermark == PageWatermark::Critical) {
          reclaimPending = reclaimLowest();
          if (!reclaimPending) { pr_debug("MEM reclaim no eligible process\n"); }
        }
      }
      observeWatermark();
    }
    const u32 budget = readyCount();
    StepResult result = StepResult::NoDispatch;
    u32 attempts = 0;
    while (attempts < budget) {
      cli();
      Thread* t = takeReady();
      if (!t) { sti(); break; }
      attempts++;
      Process* p = t->process;
      if (p->state != 1) { panic("Retired process in ready queue", p->pid, 0); }
      current = p;
      currentThread = (int)t->index;
      user_switch_out = 0;
      paging_activate(p->pd);
      bool ready = true;
      if (t->anonFaultPage) {
        ready = retryAnonFault(*t);
      } else if (t->waitOp != WaitReason::NONE) {
        ready = retryWait(t);
        if (!ready) { task_schedule_stats.retriesBlocked++; }
      }
      if (ready) {
        quantum_end = ticks + 2;
        setEntryStack(*p);
        process_switches++;
        user_enter(&t->saved);
        task_schedule_stats.dispatches++;
        result = StepResult::Ran;
      }
      paging_activate_kernel();
      current = nullptr;
      currentThread = -1;
      if (p->state == 1 && t->state == ThreadState::RUNNING) { enqueueReady(*t); }
      requireUnbound(false);
      task_schedule_stats.cleanups++;
      sti();
      if (ready) { break; }
    }
    task_schedule_stats.candidates += attempts;
    if (attempts > task_schedule_stats.maxCandidates) {
      task_schedule_stats.maxCandidates = attempts;
    }
    if (result == StepResult::NoDispatch) { task_schedule_stats.noDispatch++; }
    requireUnbound(true);
    return result;
  }
}
