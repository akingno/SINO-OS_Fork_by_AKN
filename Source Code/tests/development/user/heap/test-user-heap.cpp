#include "../../../../user/heap.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <malloc.h>

namespace {

constexpr u32 PageBytes = 4096;
constexpr u32 SlabBytes = 64 * 1024;
constexpr u8 BackingPattern = 0xA7;

struct Mappings {
  void* bases[ANON_MAX_REGIONS]{};
  u32 mapCalls = 0;
  u32 unmapCalls = 0;
  u32 live = 0;
  u32 lastRequest = 0;
  void* lastBase = nullptr;
  bool failMap = false;
  bool failUnmap = false;

  ~Mappings() {
    for (void* base : bases) {
      if (base) { _aligned_free(base); }
    }
  }

  sino::heap::MappingBackend backend() { return {this, map, unmap}; }

  static sino::heap::MapResult map(void* context, u32 bytes) {
    auto& state = *static_cast<Mappings*>(context);
    ++state.mapCalls;
    state.lastRequest = bytes;
    if (state.failMap) {
      state.failMap = false;
      return {nullptr, S_NOMEM};
    }
    for (void*& base : state.bases) {
      if (base) { continue; }
      const u32 rounded = (bytes + PageBytes - 1) & ~(PageBytes - 1);
      base = _aligned_malloc(rounded, PageBytes);
      assert(base);
      std::memset(base, BackingPattern, rounded);
      state.lastBase = base;
      ++state.live;
      return {base, S_OK};
    }
    return {nullptr, S_LIMIT};
  }

  static int unmap(void* context, void* pointer) {
    auto& state = *static_cast<Mappings*>(context);
    ++state.unmapCalls;
    if (state.failUnmap) {
      state.failUnmap = false;
      return S_IO;
    }
    for (void*& base : state.bases) {
      if (base == pointer) {
        _aligned_free(base);
        base = nullptr;
        --state.live;
        return S_OK;
      }
    }
    return S_INVALID;
  }
};

u8* allocate(sino::heap::Heap& heap, u32 bytes) {
  auto* pointer = static_cast<u8*>(heap.allocate(bytes));
  assert(pointer && reinterpret_cast<__UINTPTR_TYPE__>(pointer) % 16 == 0);
  return pointer;
}

void checkBytes(const u8* pointer, u32 bytes, u8 expected) {
  for (u32 i = 0; i < bytes; ++i) {
    assert(pointer[i] == expected);
  }
}

#ifndef HEAP_DEBUG

void denseSmallAllocations() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  u8* objects[100];
  for (u32 i = 0; i < 100; ++i) {
    objects[i] = allocate(heap, 32);
    if (i) { assert(objects[i] == objects[i - 1] + 32); }
    checkBytes(objects[i], 32, BackingPattern);
    std::memset(objects[i], i, 32);
  }
  assert(mappings.mapCalls == 1 && mappings.lastRequest == SlabBytes);
  for (u32 i = 0; i < 100; ++i) {
    checkBytes(objects[i], 32, static_cast<u8>(i));
    assert(heap.release(objects[i]));
  }
  assert(mappings.unmapCalls == 1 && mappings.live == 0);
}

void fullSlabReuse() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  // Two full 4080-byte slabs, so freeing from the first removes a non-head list node.
  void* objects[32];
  for (void*& object : objects) { object = allocate(heap, 4080); }
  assert(mappings.mapCalls == 2 && mappings.live == 2);
  mappings.failMap = true;
  assert(!heap.allocate(4080) && mappings.mapCalls == 3 && mappings.live == 2);
  assert(heap.release(objects[0]));
  assert(heap.allocate(4080) == objects[0] && mappings.mapCalls == 3);
  for (void* object : objects) { assert(heap.release(object)); }
  assert(mappings.unmapCalls == 2 && mappings.live == 0);
}

void mixedClasses() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  constexpr u32 Sizes[] = {16, 32, 64, 128, 256, 496, 1008, 2032, 4080};
  u8* first[9];
  u8* second[9];
  u32 request = 1;
  for (u32 i = 0; i < 9; ++i) {
    first[i] = allocate(heap, request);
    second[i] = allocate(heap, Sizes[i]);
    assert(second[i] == first[i] + Sizes[i]);
    std::memset(first[i], i + 1, Sizes[i]);
    std::memset(second[i], i + 11, Sizes[i]);
    request = Sizes[i] + 1;
  }
  assert(mappings.mapCalls == 9);
  for (u32 i = 0; i < 9; ++i) {
    checkBytes(first[i], Sizes[i], static_cast<u8>(i + 1));
    checkBytes(second[i], Sizes[i], static_cast<u8>(i + 11));
    assert(heap.release(first[i]) && heap.release(second[i]));
  }
  assert(mappings.live == 0);
}

void lifoAndReleaseValidation() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* keeper = allocate(heap, 32);
  auto* first = allocate(heap, 32);
  auto* second = allocate(heap, 32);
  int foreign = 0;
  assert(!heap.release(&foreign) && !heap.release(first + 1));
  assert(!heap.release(second + 32)); // Aligned but never issued.
  assert(heap.release(first) && heap.release(second));
  assert(!heap.release(first) && !heap.release(second));
  assert(heap.allocate(32) == second && heap.allocate(32) == first && mappings.mapCalls == 1);
  assert(heap.release(first) && heap.release(second));
  *keeper = 0x91;
  mappings.failUnmap = true;
  assert(!heap.release(keeper) && *keeper == 0x91 && mappings.live == 1);
  assert(heap.release(keeper) && mappings.unmapCalls == 2 && mappings.live == 0);
}

void directMappings() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* small = allocate(heap, 4080);
  assert(mappings.lastRequest == SlabBytes);
  auto* large = allocate(heap, 4081);
  assert(mappings.lastRequest == 4081 + 16 && large == static_cast<u8*>(mappings.lastBase) + 16);
  checkBytes(large, 4081, BackingPattern);
  assert(!heap.release(large + 16) && !heap.release(mappings.lastBase));
  const auto largeAddress = reinterpret_cast<__UINTPTR_TYPE__>(large);
  *large = 0x52;
  mappings.failUnmap = true;
  assert(!heap.release(large) && *large == 0x52);
  assert(heap.release(large) && mappings.live == 1);
  assert(!heap.release(reinterpret_cast<void*>(largeAddress)));

  void* direct[ANON_MAX_REGIONS - 1];
  for (void*& block : direct) { block = allocate(heap, 8192); }
  const u32 calls = mappings.mapCalls;
  assert(!heap.allocate(8192) && mappings.mapCalls == calls);
  auto* extraSmall = allocate(heap, 4080);
  assert(mappings.mapCalls == calls);
  assert(heap.release(direct[0]));
  direct[0] = allocate(heap, 8192);
  for (void* block : direct) { assert(heap.release(block)); }
  assert(heap.release(extraSmall) && heap.release(small) && mappings.live == 0);
}

void zeroedAllocationAndBounds() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  assert(heap.release(nullptr) && !heap.allocate(0) && !heap.allocate(~0u));
  assert(!heap.allocate(ANON_MAX_BYTES - 15) && mappings.mapCalls == 0);
  assert(!heap.allocateZeroed(0, 32) && !heap.allocateZeroed(32, 0));
  assert(!heap.allocateZeroed(0x80000000u, 2) && mappings.mapCalls == 0);
  mappings.failMap = true;
  assert(!heap.allocate(ANON_MAX_BYTES - 16) && mappings.lastRequest == ANON_MAX_BYTES);

  auto* keeper = allocate(heap, 32);
  auto* dirty = allocate(heap, 32);
  std::memset(dirty, 0x62, 32);
  assert(heap.release(dirty));
  auto* zeroed = static_cast<u8*>(heap.allocateZeroed(3, 7));
  assert(zeroed == dirty);
  checkBytes(zeroed, 21, 0);
  assert(zeroed[21] == 0x62);
  assert(heap.release(zeroed) && heap.release(keeper));

  auto* large = static_cast<u8*>(heap.allocateZeroed(3, 1400));
  assert(large && mappings.lastRequest == 4200 + 16);
  checkBytes(large, 4200, 0);
  assert(heap.release(large) && mappings.live == 0);
}

#else

void releaseScrub() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* keeper = allocate(heap, 32);
  auto* slot = allocate(heap, 32);
  assert(heap.release(slot));
  checkBytes(slot + sizeof(u32), 32 - sizeof(u32), 0xED);
  checkBytes(keeper, 32, 0xDC);
  assert(heap.release(keeper) && mappings.live == 0);
}

void allocationScrub() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* first = allocate(heap, 32);
  auto* slot = allocate(heap, 32);
  checkBytes(slot, 32, 0xDC);
  assert(heap.release(slot) && heap.allocate(32) == slot);
  checkBytes(slot, 32, 0xDC);
  auto* large = allocate(heap, 4200);
  checkBytes(large, 4200, BackingPattern);
  assert(heap.release(first) && heap.release(slot) && heap.release(large) && mappings.live == 0);
}

#endif

} // namespace

int main() {
#ifndef HEAP_DEBUG
  denseSmallAllocations();
  std::puts("PASS user.heap.dense-small-allocations");
  fullSlabReuse();
  std::puts("PASS user.heap.full-slab-reuse");
  mixedClasses();
  std::puts("PASS user.heap.mixed-size-classes");
  lifoAndReleaseValidation();
  std::puts("PASS user.heap.lifo-and-release-validation");
  directMappings();
  std::puts("PASS user.heap.direct-mappings");
  zeroedAllocationAndBounds();
  std::puts("PASS user.heap.zeroed-allocation-and-bounds");
#else
  releaseScrub();
  std::puts("PASS user.heap.release-scrub");
  allocationScrub();
  std::puts("PASS user.heap.allocation-scrub");
#endif
}
