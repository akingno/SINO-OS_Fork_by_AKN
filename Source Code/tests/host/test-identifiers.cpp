#include "../../src32/identifiers.h"
#include <cstdio>
#include <cstdlib>
static void check(bool ok) { if (!ok) { std::abort(); } }
int main() {
  {
    ids::ProcessPool<8, 4, 3> pool{};
    for (u32 round = 1; round <= 3; round++) {
      u32 tokens[8]{};
      for (u32 i = 0; i < 8; i++) {
        check(pool.reserve() == (int)i);
        tokens[i] = pool.commit(i);
        check(tokens[i] == (round << 4 | (i + 1)) && pool.lookup(tokens[i]) == (int)i);
      }
      check(pool.reserve() == -1);
      for (u32 token : tokens) { check(pool.release(token) && !pool.release(token)); }
    }
    check(pool.reserve() == -1);
    std::puts("PASS identifiers.pid.capacity-reuse-and-retirement");
  }
  {
    ids::ProcessPool<8, 4, 1> pool{};
    for (u32 i = 0; i < 100; i++) {
      check(pool.reserve() == 0 && pool.lookup(17) == -1 && pool.abort(0) && !pool.abort(0));
    }
    check(pool.reserve() == 0 && pool.commit(0) == 17 && !pool.commit(0) && !pool.abort(0));
    check(pool.release(17) && pool.reserve() == 1);
    std::puts("PASS identifiers.pid.reservation-rollback-without-consumption");
  }
  {
    ids::ProcessPool<8, 4, 0x07ffffffu> pool{};
    check(pool.reserve() == 0);
    const u32 old = pool.commit(0);
    check(pool.release(old) && pool.reserve() == 0);
    const u32 next = pool.commit(0);
    const u32 invalid[] = {0u, 1u, 16u, old, next ^ 8u, next | 0x80000000u, 0xffffffffu};
    for (u32 bad : invalid) {
      check(pool.lookup(bad) == -1 && !pool.release(bad));
    }
    check(pool.lookup(next) == 0 && pool.release(next));
    pool.slots[7].generation.value = 0x07fffffeu;
    for (u32 i = 0; i < 7; i++) { pool.slots[i].state = ids::State::Retired; }
    check(pool.reserve() == 7 && pool.commit(7) == 0x7ffffff8u);
    check(pool.release(0x7ffffff8u) && pool.reserve() == -1);
    std::puts("PASS identifiers.pid.stale-malformed-and-positive-boundary");
  }
  {
    ids::Cookies<24, 2> cookies{};
    for (u32 round = 1; round <= 2; round++) {
      for (u32 i = 0; i < 24; i++) {
        check(cookies.choose(0x00ffffffu) == (int)i && cookies.issue(i) == round);
      }
    }
    check(cookies.choose(0x00ffffffu) == -1 && !cookies.issue(24) && !cookies.issue(0));
    std::puts("PASS identifiers.cookie.balanced-global-slot-and-retirement");
  }
  {
    ids::Cookies<24, 0x7fffffu> cookies{};
    check(cookies.choose(0) == -1);
    for (auto& c : cookies.counters) { c.value = 0x7fffffu; }
    cookies.counters[23].value--;
    check(cookies.choose(1) == -1 && cookies.choose(1u << 23) == 23);
    check(((cookies.issue(23) << 8) | 24) == 0x7fffff18u);
    check(cookies.choose(0x00ffffffu) == -1);
    std::puts("PASS identifiers.cookie.occupied-slots-and-positive-boundary");
  }
  {
    ids::Counter<0xffffffffu> object{0xfffffffeu};
    ids::Counter<0x0fffffffu> thread{0x0ffffffeu};
    check(object.issue() == 0xffffffffu && !object.available() && !object.issue());
    check(thread.issue() == 0x0fffffffu && !thread.available() && !thread.issue());
    check((thread.value << 3 | 4) == 0x7ffffffcu);
    std::puts("PASS identifiers.object-tid.full-width-nonwrapping-counters");
  }
}
