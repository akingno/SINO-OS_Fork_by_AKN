#include "../../user/heap.h"
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {

constexpr u32 PageBytes = 4096;
constexpr u32 SlabBytes = 64 * 1024;
constexpr u8 BackingPattern = 0xA7;

struct Mappings {
  struct Region {
    void* base = nullptr;
    u32 bytes = 0;
    bool purgeable = false;
    bool discarded = false;
  } regions[ANON_MAX_REGIONS];
  std::vector<void*> unmapped;
  u32 mapCalls = 0;
  u32 unmapCalls = 0;
  u32 live = 0;
  u32 mappedBytes = 0;
  u32 regionLimit = ANON_MAX_REGIONS;
  u32 byteLimit = ANON_MAX_BYTES;
  u32 lastRequest = 0;
  void* lastBase = nullptr;
  bool failMap = false;
  bool failUnmap = false;
  bool failMark = false;
  bool failClear = false;
  bool expectFatal = false;
  void* fatalBase = nullptr;
  int fatalStatus = S_OK;
  std::jmp_buf fatalTarget;

  ~Mappings() {
    for (const auto& region : regions) {
      if (region.base) { assert(VirtualFree(region.base, 0, MEM_RELEASE)); }
    }
  }

  sino::heap::MappingBackend backend() { return {this, map, unmap, setPurgeable, fatalUnmap}; }

  Region* find(const void* base) {
    if (!base) { return nullptr; }
    for (auto& region : regions) {
      if (region.base == base) { return &region; }
    }
    return nullptr;
  }

  void* baseOf(const void* pointer) const {
    const auto address = reinterpret_cast<__UINTPTR_TYPE__>(pointer);
    for (const auto& region : regions) {
      const auto base = reinterpret_cast<__UINTPTR_TYPE__>(region.base);
      if (region.base && address >= base && address - base < region.bytes) { return region.base; }
    }
    return nullptr;
  }

  void discard(void* base) {
    auto* region = find(base);
    assert(region && region->purgeable && !region->discarded);
    assert(VirtualFree(base, region->bytes, MEM_DECOMMIT));
    region->discarded = true;
  }

  static sino::heap::MapResult map(void* context, u32 bytes) {
    auto& state = *static_cast<Mappings*>(context);
    ++state.mapCalls;
    state.lastRequest = bytes;
    if (state.failMap) {
      state.failMap = false;
      return {nullptr, S_NOMEM};
    }
    const u32 rounded = (bytes + PageBytes - 1) & ~(PageBytes - 1);
    if (state.live >= state.regionLimit || state.mappedBytes > state.byteLimit ||
        rounded > state.byteLimit - state.mappedBytes) {
      return {nullptr, S_LIMIT};
    }
    for (auto& region : state.regions) {
      if (region.base) { continue; }
      void* base = VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      assert(base);
      std::memset(base, BackingPattern, rounded);
      region = {base, rounded};
      state.lastBase = base;
      ++state.live;
      state.mappedBytes += rounded;
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
    auto* region = state.find(pointer);
    if (!region) { return S_INVALID; }
    assert(VirtualFree(pointer, 0, MEM_RELEASE));
    state.unmapped.push_back(pointer);
    state.mappedBytes -= region->bytes;
    *region = {};
    --state.live;
    return S_OK;
  }

  static int setPurgeable(void* context, void* base, bool enabled) {
    auto& state = *static_cast<Mappings*>(context);
    auto* region = state.find(base);
    assert(region && region->purgeable != enabled);
    bool& fail = enabled ? state.failMark : state.failClear;
    if (fail) {
      fail = false;
      return S_IO;
    }
    if (!enabled && region->discarded) {
      assert(VirtualAlloc(base, region->bytes, MEM_COMMIT, PAGE_READWRITE) == base);
      region->discarded = false;
    }
    region->purgeable = enabled;
    return S_OK;
  }

  static void fatalUnmap(void* context, void* base, int status) {
    auto& state = *static_cast<Mappings*>(context);
    assert(state.expectFatal && state.find(base));
    state.fatalBase = base;
    state.fatalStatus = status;
    std::longjmp(state.fatalTarget, 1);
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
  assert(mappings.unmapCalls == 0 && mappings.live == 1);
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
  assert(mappings.unmapCalls == 0 && mappings.live == 2);
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
  assert(mappings.live == 9 && mappings.unmapCalls == 0);
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
  assert(heap.release(keeper) && !heap.release(keeper) && mappings.unmapCalls == 0);
  assert(heap.allocate(32) == keeper && mappings.mapCalls == 1);
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
  assert(heap.release(extraSmall) && heap.release(small) && mappings.live == 1);
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
  assert(heap.release(large) && mappings.live == 1);
}

void reallocNull() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  assert(!heap.reallocate(nullptr, 0));
  void* pointer = heap.reallocate(nullptr, 49);
  assert(pointer && heap.usableSize(pointer) == 64 && mappings.mapCalls == 1);
  assert(heap.release(pointer));
}

void reallocZero() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  void* small = allocate(heap, 16);
  void* large = allocate(heap, 8192);
  assert(!heap.reallocate(small, 0) && !heap.usableSize(small));
  assert(!heap.reallocate(large, 0) && mappings.unmapCalls == 1 && mappings.live == 1);
  assert(heap.allocate(16) == small);
}

void reallocInPlace() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* pointer = allocate(heap, 20);
  std::memset(pointer, 0x53, 20);
  assert(heap.reallocate(pointer, 9) == pointer);
  assert(heap.reallocate(pointer, 32) == pointer && mappings.mapCalls == 1);
  checkBytes(pointer, 20, 0x53);
  assert(heap.release(pointer));
}

void reallocGrowth() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* small = allocate(heap, 20);
  std::memset(small, 0x31, 20);
  auto* larger = static_cast<u8*>(heap.reallocate(small, 8193));
  assert(larger && larger != small && !heap.usableSize(small));
  checkBytes(larger, 20, 0x31);
  std::memset(larger, 0x92, 8193);
  auto* largest = static_cast<u8*>(heap.reallocate(larger, 12289));
  assert(largest && largest != larger && heap.usableSize(largest) == 12289);
  checkBytes(largest, 8193, 0x92);
  assert(mappings.unmapCalls == 1 && heap.release(largest));
}

void reallocFailure() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* original = allocate(heap, 20);
  mappings.regionLimit = 1;
  std::memset(original, 0x31, 20);
  assert(!heap.reallocate(original, 8192) && heap.usableSize(original) == 32);
  assert(!heap.reallocate(original, ~0u) && mappings.mapCalls == 2 && mappings.live == 1);
  assert(!heap.reallocate(original + 1, 400));
  checkBytes(original, 20, 0x31);
  assert(heap.release(original));
}

constexpr u32 SlotsPerLargeSlab = 16;
using SlabBatch = u8*[SlotsPerLargeSlab];

void* fillLargeSlab(Mappings& mappings, sino::heap::Heap& heap, SlabBatch& batch) {
  for (auto& pointer : batch) { pointer = allocate(heap, 4080); }
  return mappings.baseOf(batch[0]);
}

void releaseBatch(sino::heap::Heap& heap, const SlabBatch& batch) {
  for (auto* pointer : batch) { assert(heap.release(pointer)); }
}

void activeThenHot() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  SlabBatch first, second;
  fillLargeSlab(mappings, heap, first);
  fillLargeSlab(mappings, heap, second);
  releaseBatch(heap, first);
  assert(heap.release(second[0]));
  assert(heap.allocate(4080) == second[0]);
  assert(heap.allocate(4080) == first[SlotsPerLargeSlab - 1]);
  assert(mappings.mapCalls == 2 && mappings.unmapCalls == 0);
}

void cacheLimits() {
  {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    SlabBatch first, second;
    auto* firstBase = fillLargeSlab(mappings, heap, first);
    auto* secondBase = fillLargeSlab(mappings, heap, second);
    u8* other[] = {allocate(heap, 16), allocate(heap, 32), allocate(heap, 64), allocate(heap, 128)};
    releaseBatch(heap, first);
    releaseBatch(heap, second);
    assert(mappings.find(firstBase)->purgeable && !mappings.find(secondBase)->purgeable);
    for (auto* pointer : other) { assert(heap.release(pointer)); }
    assert(mappings.find(secondBase)->purgeable);
    for (auto* pointer : other) { assert(!mappings.find(mappings.baseOf(pointer))->purgeable); }
    assert(mappings.live == 6 && mappings.unmapCalls == 0);
  }
  {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    constexpr u32 Count = 16;
    SlabBatch batches[Count];
    void* bases[Count];
    for (u32 i = 0; i < Count; ++i) { bases[i] = fillLargeSlab(mappings, heap, batches[i]); }
    for (const auto& batch : batches) { releaseBatch(heap, batch); }
    assert(mappings.live == 13 && mappings.mappedBytes == 13 * SlabBytes);
    assert(mappings.unmapped == std::vector<void*>(bases, bases + 3));
    for (u32 i = 3; i < Count - 1; ++i) { assert(mappings.find(bases[i])->purgeable); }
    assert(!mappings.find(bases[Count - 1])->purgeable);
  }
}

void cacheRetryOne() {
  for (u32 bytes : {64u, 8192u}) {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    mappings.regionLimit = 1;
    auto* cached = allocate(heap, 32);
    void* base = mappings.baseOf(cached);
    assert(heap.release(cached));
    assert(allocate(heap, bytes) && mappings.mapCalls == 3 && mappings.live == 1);
    assert(mappings.unmapped.size() == 1 && mappings.unmapped[0] == base);
  }
  {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    auto* cached = allocate(heap, 32);
    void* cachedBase = mappings.baseOf(cached);
    assert(heap.release(cached));
    for (u32 i = 1; i < ANON_MAX_REGIONS; ++i) { allocate(heap, 8192); }
    assert(mappings.mapCalls == ANON_MAX_REGIONS);
    // Full internal registry: evict before calling the backend.
    assert(allocate(heap, 8192) && mappings.mapCalls == ANON_MAX_REGIONS + 1);
    assert(mappings.unmapped.size() == 1 && mappings.unmapped[0] == cachedBase);
  }
}

void usableSizes() {
  Mappings mappings;
  sino::heap::Heap heap(mappings.backend());
  auto* first = allocate(heap, 23);
  auto* second = allocate(heap, 31);
  auto* large = allocate(heap, 4099);
  assert(heap.usableSize(first) == 32 && heap.usableSize(second) == 32);
  assert(heap.usableSize(large) == 4099 && !heap.usableSize(large + 16));
  assert(!heap.usableSize(nullptr) && !heap.usableSize(first + 1));
  assert(heap.release(first) && !heap.usableSize(first));
  assert(heap.release(second) && !heap.usableSize(second));
}

void coldRebuild() {
  for (bool discardFirst : {false, true}) {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    SlabBatch first, second;
    void* base = fillLargeSlab(mappings, heap, first);
    fillLargeSlab(mappings, heap, second);
    auto* keeper = allocate(heap, 32);
    std::memset(keeper, 0x75, 32);
    releaseBatch(heap, first);
    releaseBatch(heap, second);
    if (discardFirst) { mappings.discard(base); }
    assert(!heap.usableSize(first[0]) && !heap.release(first[0]));

    u8* rebuilt[32];
    for (auto& pointer : rebuilt) {
      pointer = allocate(heap, 2032);
      assert(mappings.baseOf(pointer) == base && heap.usableSize(pointer) == 2032);
    }
    assert(mappings.mapCalls == 3 && !mappings.find(base)->purgeable);
    auto* overflow = allocate(heap, 2032);
    assert(mappings.baseOf(overflow) != base);
    for (auto* pointer : rebuilt) { assert(heap.release(pointer)); }
    assert(heap.release(overflow) && mappings.find(base)->purgeable);
    mappings.discard(base);
    auto* again = allocate(heap, 256);
    assert(mappings.baseOf(again) == base && heap.usableSize(again) == 256 && mappings.mapCalls == 4);
    checkBytes(keeper, 32, 0x75);
  }
}

void cacheRetryDrain() {
  for (bool succeeds : {true, false}) {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    SlabBatch batches[3];
    void* bases[3];
    for (u32 i = 0; i < 3; ++i) { bases[i] = fillLargeSlab(mappings, heap, batches[i]); }
    auto* keeper = allocate(heap, 8192);
    std::memset(keeper, 0xB2, 8192);
    for (const auto& batch : batches) { releaseBatch(heap, batch); }
    mappings.discard(bases[0]);
    mappings.byteLimit = mappings.mappedBytes;
    const u32 request = (succeeds ? 2u : 4u) * SlabBytes - 16;
    const u32 calls = mappings.mapCalls;
    void* result = heap.allocate(request);
    assert((result != nullptr) == succeeds && mappings.mapCalls == calls + 3);
    assert(mappings.unmapped == std::vector<void*>(bases, bases + 3));
    assert(heap.usableSize(keeper) == 8192);
    checkBytes(keeper, 8192, 0xB2);
  }
}

void checkFatalUnmap(Mappings& mappings, sino::heap::Heap& heap, void* expectedBase) {
  mappings.expectFatal = true;
  mappings.failUnmap = true;
  if (!setjmp(mappings.fatalTarget)) {
    heap.allocate(8192);
    assert(false && "an unexpected unmap failure must reach the fatal callback");
  }
  mappings.expectFatal = false;
  assert(mappings.fatalStatus == S_IO && mappings.fatalBase == expectedBase);
}

void cacheFailurePaths() {
  for (bool failMark : {true, false}) {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    SlabBatch first, second;
    void* firstBase = fillLargeSlab(mappings, heap, first);
    void* secondBase = fillLargeSlab(mappings, heap, second);
    releaseBatch(heap, first);
    mappings.failMark = failMark;
    releaseBatch(heap, second);
    if (!failMark) {
      mappings.discard(firstBase);
      mappings.failClear = true;
      assert(allocate(heap, 64));
    }
    assert(mappings.unmapped.size() == 1 && mappings.unmapped[0] == firstBase);
    assert(mappings.find(secondBase) && !mappings.find(secondBase)->purgeable);
  }
  {
    Mappings mappings;
    sino::heap::Heap heap(mappings.backend());
    auto* pointer = allocate(heap, 32);
    void* base = mappings.baseOf(pointer);
    assert(heap.release(pointer));
    mappings.regionLimit = 1;
    checkFatalUnmap(mappings, heap, base);
    assert(mappings.live == 1 && mappings.unmapped.empty());
    assert(heap.allocate(32) == pointer && mappings.mapCalls == 2);
  }
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
  assert(heap.release(keeper) && mappings.live == 1);
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
  assert(heap.release(first) && heap.release(slot) && heap.release(large) && mappings.live == 1);
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
  reallocNull();
  std::puts("PASS user.heap.realloc-null");
  reallocZero();
  std::puts("PASS user.heap.realloc-zero");
  reallocInPlace();
  std::puts("PASS user.heap.realloc-in-place");
  reallocGrowth();
  std::puts("PASS user.heap.realloc-growth");
  reallocFailure();
  std::puts("PASS user.heap.realloc-failure");
  activeThenHot();
  std::puts("PASS user.heap.active-then-hot");
  cacheLimits();
  std::puts("PASS user.heap.cache-limits");
  cacheRetryOne();
  std::puts("PASS user.heap.cache-retry-one");
  usableSizes();
  std::puts("PASS user.heap.usable-sizes");
  coldRebuild();
  std::puts("PASS user.heap.cold-rebuild");
  cacheRetryDrain();
  std::puts("PASS user.heap.cache-retry-drain");
  cacheFailurePaths();
  std::puts("PASS user.heap.cache-failure-paths");
#else
  releaseScrub();
  std::puts("PASS user.heap.release-scrub");
  allocationScrub();
  std::puts("PASS user.heap.allocation-scrub");
#endif
}
