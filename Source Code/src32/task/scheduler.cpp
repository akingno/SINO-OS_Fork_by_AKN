#include "internal.h"
#include "../klog.h"
#include "../net.h"
#include "../panic.h"

namespace task {
  namespace {
    u32 cursor, quantum_end;
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
        int status = anonPage(current->pd, page_floor(address), 1);
        if (status == S_OK) {
          return 1;
        }
        if (status == S_NOMEM) {
          pr_debug("USER anonymous page OOM address=");
          pr_debug_hex(address);
          pr_debug("\n");
        }
      }
      process_faults++;
      current->code = -(int)f->vector;
      current->reason = PROCESS_FAULTED;
      current->fault_vector = f->vector;
      current->state = 2;
      pr_debug("USER fault contained pid=");
      pr_debug_hex(current->pid);
      pr_debug(" vector=");
      pr_debug_hex(f->vector);
      pr_debug("\n");
      leave(f);
      return 1;
    }
    if (f->vector == 0x80) {
      if (current->caps & CAP_NETWORK) {
        net_poll();
      }
      int r = dispatchSyscall(f);
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
  /* 【简化实现：调度器】由内核主循环调用，扫描固定槽位处理退出/超时，按线程
  * 轮转执行一次，时间片为 2 tick（当前约 20 ms），之后返回主循环。
  * 公平单位是线程槽而非进程；后台工作和内核长任务会影响用户线程的实际延迟。
  * 替换方向：独立的就绪队列、定时等待队列与空闲路径，明确公平策略，
  * 为后台工作保留推进机会；优先完成生命周期与等待语义，再增加优先级。 */
  void step(void) {
    reapAndExpireRuns();
    u32 now = ticks;
    for (int i = 0; i < MaxProcesses; i++) {
      Process* p = &processes[i];
      if (p->state != 1) {
        continue;
      }
      for (int j = 0; j < MaxThreads; j++) {
        Thread* t = &p->thread[j];
        if (t->waitExpired(now)) {
          t->finishWait(S_TIMEOUT, now);
        }
      }
    }
    for (int i = 0; i < MaxProcesses * MaxThreads; i++) {
      int which = cursor++ % (MaxProcesses * MaxThreads);
      Process* p = &processes[which / MaxThreads];
      Thread* t = &p->thread[which % MaxThreads];
      if (p->state != 1 || t->state != ThreadState::ACTIVE || (i32)(ticks - t->wake) < 0) {
        continue;
      }
      current = p;
      currentThread = which % MaxThreads;
      user_switch_out = 0;
      cli();
      paging_activate(p->pd);
      int ready = t->waitOp == WaitReason::NONE || retryWait(t);
      if (ready) {
        quantum_end = ticks + 2;
        setEntryStack(*p);
        process_switches++;
        user_enter(&t->saved);
      }
      paging_activate_kernel();
      current = 0;
      sti();
      if (ready) {
        break;
      }
    }
  }

}
