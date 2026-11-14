#include "../../src32/task/collections.h"
#include <cstdio>
#include <cstdlib>

using namespace task;
struct Node {
  QueueLink<Node> link;
  QueueLink<Node>& queueLink() { return link; }
};
static void check(bool ok) { if (!ok) { std::abort(); } }
int main() {
  {
    IntrusiveQueue<Node> a{}, b{};
    Node nodes[96]{};
    for (auto& n : nodes) { check(a.push(n) && !a.push(n) && !b.push(n)); }
    for (u32 i = 0; i < 96; i += 2) { check(a.erase(nodes[i]) && !a.erase(nodes[i])); }
    for (u32 i = 1; i < 96; i += 2) { check(a.pop() == &nodes[i]); }
    check(a.empty() && !a.pop() && !a.size());
    for (auto& n : nodes) { check(b.push(n)); }
    for (auto& n : nodes) { check(b.pop() == &n); }
    std::puts("PASS task.queues.fifo-ownership-and-revoke-96");
  }
  {
    TimerHeap<96> heap{};
    TimerNode nodes[97]{};
    for (u32 i = 0; i < 96; i++) { check(heap.insert(nodes[i], 1000 - i)); }
    check(!heap.insert(nodes[96], 1) && !heap.insert(nodes[0], 1));
    for (u32 i = 0; i < 96; i += 2) { check(heap.erase(nodes[i]) && !heap.erase(nodes[i])); }
    for (u32 i = 95; i < 96; i -= 2) { check(heap.pop() == &nodes[i]); }
    check(!heap.size() && !heap.pop());
    std::puts("PASS task.queues.timer-capacity-and-cancel-96");
  }
  {
    TimerHeap<96> heap{};
    TimerNode nodes[96]{};
    constexpr u32 start = 0xfffffff0u;
    for (u32 i = 0; i < 96; i++) { check(heap.insert(nodes[i], start + i / 3)); }
    for (u32 i = 0; i < 96; i++) {
      check(heap.due(start + i / 3) && heap.pop() == &nodes[i]);
    }
    check(!heap.due(0));
    std::puts("PASS task.queues.timer-wrap-and-fifo-ties");
  }
  {
    TimerHeap<128> heap{};
    TimerNode nodes[96]{};
    for (u32 round = 0; round < 20; round++) {
      for (u32 i = 0; i < 96; i++) { check(heap.insert(nodes[i], (i * 37) % 96)); }
      for (u32 i = 0; i < 96; i += 3) { check(heap.erase(nodes[i])); }
      for (u32 i = 0; i < 96; i += 3) { check(heap.insert(nodes[i], 200 + i)); }
      u32 previous = 0;
      while (auto* n = heap.pop()) {
        check(n->deadline >= previous && !n->slot);
        previous = n->deadline;
      }
      check(heap.size() == 0);
    }
    std::puts("PASS task.queues.timer-revoke-and-reinsert");
  }
}
