#include "internal.h"
#include "../sync/irq.h"
#include "../pipe.h"
#include "../panic.h"
#include "../lib/kstring.h"

namespace task {
namespace {
IntrusiveQueue<Thread> ready;
IntrusiveQueue<Process> reaping;
TimerHeap<MaxProcesses * (MaxThreads + 1)> timers;
struct WaitBucket {
  u32 generation, inFlight;
  IntrusiveQueue<Thread> waiters;
};
WaitBucket buckets[OBJECTS][(u32)WaitReason::Count];

WaitBucket& bucket(WaitKey key) {
  if (!key.object || key.object > OBJECTS || key.reason == WaitReason::NONE ||
      key.reason >= WaitReason::Count) {
    panic("Invalid wait key", key.object, (u32)key.reason);
  }
  return buckets[key.object - 1][(u32)key.reason];
}
void pushReady(Thread& t) {
  if (!t.process || t.process->state != 1 || !ready.push(t)) {
    panic("Invalid ready enqueue", (u32)t.state, t.index);
  }
  t.state = ThreadState::READY;
}
void unlink(Thread& t) {
  if (t.schedLink.owner) {
    t.schedLink.owner->erase(t);
  }
}
void dropTicket(Thread& t) {
  if (t.waitNotified) {
    WaitBucket& b = bucket(t.waitKey());
    if (b.generation == t.waitGeneration) {
      if (!b.inFlight) { panic("Wait notification underflow", t.waitObject, 0); }
      b.inFlight--;
    }
    t.waitNotified = false;
  }
}
WaitKey clearWait(Thread& t) {
  WaitKey key = t.waitKey();
  unlink(t);
  disarmTimer(t.timer);
  dropTicket(t);
  t.waitObject = t.waitGeneration = t.waitDeadline = 0;
  t.waitTimed = 0;
  t.waitOp = WaitReason::NONE;
  return key;
}
}

void enqueueReady(Thread& t) {
  sync::IrqGuard guard;
  pushReady(t);
}
Thread* takeReady() {
  sync::IrqGuard guard;
  Thread* t = ready.pop();
  if (t) { t->state = ThreadState::RUNNING; }
  return t;
}
u32 readyCount() { sync::IrqGuard guard; return ready.size(); }
void armTimer(TimerNode& node, u32 deadline, TimerKind kind, void* target) {
  sync::IrqGuard guard;
  node.kind = kind;
  node.target = target;
  if (!timers.insert(node, deadline)) {
    panic("Timer duplicate/capacity", node.slot, timers.size());
  }
}
void disarmTimer(TimerNode& node) {
  sync::IrqGuard guard;
  if (node.slot && !timers.erase(node)) {
    panic("Timer ownership", node.slot, timers.size());
  }
}
bool queuedWork(u32 now) { return !ready.empty() || !reaping.empty() || timers.due(now); }
Process* takeReap() { sync::IrqGuard guard; return reaping.pop(); }

void objectCreated(u32 object) {
  sync::IrqGuard guard;
  for (u32 reason = 1; reason < (u32)WaitReason::Count; reason++) {
    WaitBucket& b = buckets[object - 1][reason];
    if (!b.waiters.empty()) { panic("Reused object has waiters", object, reason); }
    b = {};
    b.generation = objects[object - 1].generation;
  }
}
void notify(u32 object, WaitReason reason, bool all) {
  if (!object || object > OBJECTS) { return; }
  sync::IrqGuard guard;
  WaitKey key{object, objects[object - 1].generation, reason};
  WaitBucket& b = bucket(key);
  if (b.generation != key.generation || (!all && b.inFlight)) { return; }
  task_schedule_stats.notifications++;
  do {
    Thread* t = b.waiters.pop();
    if (!t) { break; }
    task_schedule_stats.wakeVisited++;
    if (t->waitGeneration != key.generation || t->waitNotified) {
      panic("Wait generation/membership", object, t->waitGeneration);
    }
    t->waitNotified = true;
    b.inFlight++;
    pushReady(*t);
    if (all) { task_schedule_stats.broadcastWakes++; }
    else { task_schedule_stats.finiteWakes++; }
  } while (all);
}
void objectClosed(u32 object) {
  for (u32 reason = 1; reason < (u32)WaitReason::Count; reason++) {
    notify(object, (WaitReason)reason, true);
  }
}
void handoff(WaitKey key) {
  if (!key.object || key.object > OBJECTS) { return; }
  const Object& o = objects[key.object - 1];
  if (!o.refs || o.generation != key.generation) { return; }
  if (key.reason == WaitReason::PIPE_READ || key.reason == WaitReason::PIPE_WRITE) {
    pipeWake(&o);
  } else if (key.reason == WaitReason::TERM_READ) {
    terminalWake(key.object);
  }
}

void Thread::block(u32 object, WaitReason op, u32 timeout) {
  sync::IrqGuard guard;
  if (state != ThreadState::RUNNING || waitOp != WaitReason::NONE ||
      !object || object > OBJECTS || !objects[object - 1].refs) {
    panic("Invalid wait registration", object, (u32)state);
  }
  waitObject = object;
  waitGeneration = objects[object - 1].generation;
  waitOp = op;
  waitTimed = timeout != 0;
  waitDeadline = ticks + timeout;
  state = ThreadState::BLOCKED;
  WaitBucket& b = bucket(waitKey());
  if (b.generation != waitGeneration || !b.waiters.push(*this)) {
    panic("Wait enqueue", object, waitGeneration);
  }
  if (waitTimed) { armTimer(timer, waitDeadline, TimerKind::Wait, this); }
}
void Thread::reblock() {
  sync::IrqGuard guard;
  dropTicket(*this);
  WaitBucket& b = bucket(waitKey());
  if (b.generation != waitGeneration || !b.waiters.push(*this)) {
    panic("Wait retry enqueue", waitObject, waitGeneration);
  }
  state = ThreadState::BLOCKED;
}
void Thread::finishWait(int result, u32 now) {
  WaitKey key;
  {
    sync::IrqGuard guard;
    const bool running = state == ThreadState::RUNNING;
    key = clearWait(*this);
    saved.frame.eax = (u32)result;
    wake = now;
    if (!running) { pushReady(*this); }
  }
  handoff(key);
}
void Thread::sleep(u32 delay) {
  sync::IrqGuard guard;
  wake = ticks + delay;
  if (delay) {
    state = ThreadState::SLEEPING;
    armTimer(timer, wake, TimerKind::Sleep, this);
  }
}
void Thread::cancel(bool pass) {
  WaitKey key;
  {
    sync::IrqGuard guard;
    key = clearWait(*this);
    state = ThreadState::UNUSED;
  }
  if (pass) { handoff(key); }
}
void detachProcess(Process& p) {
  WaitKey keys[MaxThreads];
  {
    sync::IrqGuard guard;
    disarmTimer(p.timer);
    if (p.reapLink.owner) { p.reapLink.owner->erase(p); }
    for (int i = 0; i < MaxThreads; i++) {
      keys[i] = p.thread[i].waitKey();
      p.thread[i].cancel(false);
    }
  }
  for (const auto& key : keys) { handoff(key); }
}
void Process::retire(u32 why, int result, u32 vector) {
  if (state != 1) { return; }
  state = 2;
  reason = why;
  code = result;
  fault_vector = vector;
  detachProcess(*this);
  sync::IrqGuard guard;
  if (!reaping.push(*this)) { panic("Duplicate process reap", pid, 0); }
}
void expireTimers(u32 now) {
  for (;;) {
    TimerNode* node;
    {
      sync::IrqGuard guard;
      if (!timers.due(now)) { break; }
      node = timers.pop();
    }
    task_schedule_stats.timerExpirations++;
    if (node->kind == TimerKind::Run) {
      ((Process*)node->target)->retire(PROCESS_TIMEOUT, 0);
    } else {
      Thread& t = *(Thread*)node->target;
      // TODO(等待超时语义)：这里需要重新考虑并重写通知、条件满足与超时的裁决。
      // 当前已通知进入就绪队列、尚未执行重试的线程也会被判为超时；即使数据
      // 在期限前到达，调度延迟仍可能使用户收到 S_TIMEOUT，需要评估是否合理。
      // 应区分“通知允许重试”和“成功结果已提交”，按对象语义评估条件优先或
      // 记录已获胜结果；不能简单在通知时取消定时器，否则条件失效后可能无限等。
      // 修改时同步 retryWait() 的到期检查、原期限保留、契约和回归；本轮不改语义。
      if (node->kind == TimerKind::Wait) { t.finishWait(S_TIMEOUT, now); }
      else { enqueueReady(t); }
    }
  }
}

// 真实队列/对象代数自检，不创建用户地址空间，也不额外持有端点引用。
void waitSelfTest() {
  Process& p = processes[0];
  p.state = 1;
  int index = object_new(ObjType::EVENT, 0);
  if (index < 0) { panic("Wait test object", 0, 0); }
  u32 generation = objects[index].generation;
  for (u32 i = 0; i < 2; i++) {
    Thread& t = p.thread[i];
    t.process = &p; t.index = i; t.state = ThreadState::RUNNING;
    t.block((u32)index + 1, WaitReason::COMPLETION, 0);
  }
  notify((u32)index + 1, WaitReason::COMPLETION);
  notify((u32)index + 1, WaitReason::COMPLETION);
  if (readyCount() != 1 || takeReady() != &p.thread[0]) {
    panic("Wait duplicate notification", readyCount(), 1);
  }
  object_unref(index); // 销毁广播剩余等待者，但已通知记录仍保留旧代数。
  int reused = object_new(ObjType::EVENT, 0);
  if (reused != index || objects[reused].generation == generation || readyCount() != 1) {
    panic("Wait object reuse", (u32)reused, (u32)index);
  }
  p.thread[0].cancel();
  p.thread[1].cancel();
  Thread& t = p.thread[0];
  t.state = ThreadState::RUNNING;
  t.block((u32)reused + 1, WaitReason::COMPLETION, 7);
  notify((u32)reused + 1, WaitReason::COMPLETION);
  expireTimers(ticks + 7);
  if (readyCount() != 1 || takeReady() != &t ||
      t.saved.frame.eax != (u32)S_TIMEOUT || t.timer.slot || t.waitOp != WaitReason::NONE) {
    panic("Wait timeout wins notification", readyCount(), t.timer.slot);
  }
  t.cancel();
  object_unref(reused);
  if (queuedWork(ticks)) { panic("Wait test leaked membership", 0, 0); }
  memset((void*)&p, 0, sizeof(p));
}
}
