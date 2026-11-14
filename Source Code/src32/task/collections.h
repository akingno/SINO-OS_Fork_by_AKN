#ifndef SINO_TASK_COLLECTIONS_H
#define SINO_TASK_COLLECTIONS_H
#include "../types.h"
namespace task {
template<class T> class IntrusiveQueue;
template<class T> struct QueueLink {
  T* previous;
  T* next;
  IntrusiveQueue<T>* owner;
  bool linked() const { return owner != nullptr; }
};
// 消费者持有节点；不分配内存，任意节点撤销为 O(1)。
template<class T> class IntrusiveQueue {
  T* head_;
  T* tail_;
  u32 size_;
public:
  T* front() const { return head_; }
  u32 size() const { return size_; }
  bool empty() const { return head_ == nullptr; }
  bool push(T& value) {
    auto& link = value.queueLink();
    if (link.owner) { return false; }
    link = {tail_, nullptr, this};
    if (tail_) { tail_->queueLink().next = &value; }
    else { head_ = &value; }
    tail_ = &value;
    size_++;
    return true;
  }
  bool erase(T& value) {
    auto& link = value.queueLink();
    if (link.owner != this) { return false; }
    if (link.previous) { link.previous->queueLink().next = link.next; }
    else { head_ = link.next; }
    if (link.next) { link.next->queueLink().previous = link.previous; }
    else { tail_ = link.previous; }
    link = {};
    size_--;
    return true;
  }
  T* pop() {
    T* value = head_;
    if (value) { erase(*value); }
    return value;
  }
};
enum class TimerKind : u32 { Sleep, Wait, Run };
struct TimerNode {
  u32 deadline, slot, serial;
  TimerKind kind;
  void* target;
};
// 期限距当前 tick 少于 2^31；slot = heap index + 1，零为未登记。
template<u32 Capacity> class TimerHeap {
  TimerNode* nodes_[Capacity];
  u32 size_, serial_;
  static bool earlier(const TimerNode* a, const TimerNode* b) {
    return a->deadline == b->deadline ? (i32)(a->serial - b->serial) < 0
                                      : (i32)(a->deadline - b->deadline) < 0;
  }
  void swap(u32 a, u32 b) {
    TimerNode* value = nodes_[a]; nodes_[a] = nodes_[b]; nodes_[b] = value;
    nodes_[a]->slot = a + 1; nodes_[b]->slot = b + 1;
  }
  u32 up(u32 index) {
    while (index && earlier(nodes_[index], nodes_[(index - 1) / 2])) {
      u32 parent = (index - 1) / 2;
      swap(index, parent); index = parent;
    }
    return index;
  }
  void down(u32 index) {
    while (index * 2 + 1 < size_) {
      u32 child = index * 2 + 1;
      if (child + 1 < size_ && earlier(nodes_[child + 1], nodes_[child])) { child++; }
      if (!earlier(nodes_[child], nodes_[index])) { break; }
      swap(index, child); index = child;
    }
  }
public:
  u32 size() const { return size_; }
  TimerNode* front() const { return size_ ? nodes_[0] : nullptr; }
  bool due(u32 now) const { return size_ && (i32)(now - nodes_[0]->deadline) >= 0; }
  bool insert(TimerNode& node, u32 deadline) {
    if (node.slot || size_ == Capacity) { return false; }
    node.deadline = deadline; node.serial = ++serial_;
    nodes_[size_] = &node; node.slot = ++size_;
    up(size_ - 1);
    return true;
  }
  bool erase(TimerNode& node) {
    if (!node.slot || node.slot > size_ || nodes_[node.slot - 1] != &node) { return false; }
    u32 index = node.slot - 1;
    size_--; node.slot = 0;
    if (index != size_) {
      nodes_[index] = nodes_[size_]; nodes_[index]->slot = index + 1;
      index = up(index); down(index);
    }
    nodes_[size_] = nullptr;
    return true;
  }
  TimerNode* pop() {
    TimerNode* node = front();
    if (node) { erase(*node); }
    return node;
  }
};
}
#endif
