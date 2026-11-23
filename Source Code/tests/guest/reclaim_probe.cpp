#include "stream.h"

static_assert(SYS_PROCESS_GROUP == 0x157 && FEATURE_PROCESS_GROUP == 262144u, "process group ABI");
static_assert(SYS_ANON_PURGEABLE == 0x158 && SYS_ANON_LOCK == 0x159 &&
              SYS_ANON_UNLOCK == 0x15a && FEATURE_PURGEABLE == 524288u, "purgeable ABI");
static_assert(SYS_MPROTECT == 0x15b && FEATURE_MPROTECT == 1048576u, "mprotect ABI");
static_assert(SYS_ANON_UNMAP_RANGE == 0x15c && FEATURE_ANON_UNMAP_RANGE == 4194304u, "unmap range ABI");

namespace {
constexpr u32 PageBytes = 4096;

void require(bool ok, const char* reason) {
  if (!ok) {
    Log("RECLAIM SELFTEST FAIL: ");
    Log(reason);
    Log("\n");
    Exit(1);
  }
}

[[noreturn]] void hold() {
  for (;;) { Yield(100); }
}

volatile u8* reserve(u32 pages) {
  const u32 base = AnonMap(pages * PageBytes);
  require(AnonMapStatus(base) == S_OK, "reserve anonymous memory before pressure");
  return (volatile u8*)base;
}

volatile u8* reserveInNewPageTable() {
  constexpr u32 tableBytes = 4 * 1024 * 1024;
  constexpr u32 offset = tableBytes - (USER_DYNAMIC_BASE % tableBytes);
  volatile u8* base = reserve(offset / PageBytes + 1);
  require((u32)base == USER_DYNAMIC_BASE, "table fault reservation starts at the anonymous window");
  return base + offset;
}

int queryGroup() {
  SystemInfo info;
  const int group = ProcessGroup();
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_PROCESS_GROUP) && group > 0,
          "process group feature and positive self query");
  require(api(0x157, 1, 0, 0) == S_INVALID && api(0x157, 0, 1, 0) == S_INVALID &&
          api(0x157, 0, 0, 1) == S_INVALID, "process group rejects arguments");
  return group;
}

int startGroupChild(const char* mode) {
  const u32 offset = 0;
  const RunSpec spec = {sizeof(spec), 1, 0, (u32)"/boot/reclaim.snx", 1,
                       strlen(mode) + 1, (u32)mode, (u32)&offset, 0, 0, 0, 0};
  const int child = StartRun(&spec);
  require(child > 0, "start managed group child");
  return child;
}

void logGroup(const char* label, int group) {
  char text[11];
  number((u32)group, text);
  Log(label);
  Log(text);
  Log("\n");
}

void waitForPressure(bool expected) {
  const u32 start = StreamTicks();
  for (;;) {
    const u32 mapped = AnonMap(1);
    const int status = AnonMapStatus(mapped);
    require(status == S_NOMEM || (status == S_OK && AnonUnmap((void*)mapped) == S_OK),
            "poll pressure with a temporary reservation");
    if ((status == S_NOMEM) == expected) { return; }
    require(StreamTicks() - start < 900, "pressure did not reach the requested state");
    Yield(1);
  }
}

int poolPopulate(bool denied) {
  volatile u8* memory = reserve(2);
  memory[0] = 0x4b; // Keep the page table alive while the second page is absent.
  auto* output = (SystemInfo*)(memory + PageBytes);
  Log("POOL POPULATE READY\n");
  // The host sets the pool limits before sending global LOW as the start signal.
  waitForPressure(true);
  if (denied) {
    require(QuerySystemInfo(output) == S_NOMEM, "low pool reserve denies population");
    Log("POOL POPULATE DENIED\n");
    waitForPressure(false);
  }
  require(QuerySystemInfo(output) == S_OK && output->size == sizeof(SystemInfo) &&
          memory[0] == 0x4b, "populate second page in the existing table");
  Log("POOL POPULATE DONE\n");
  if (denied) { hold(); }
  waitForPressure(false);
  require(AnonUnmap((void*)memory) == S_OK, "release the populated reservation");
  Log("POOL POPULATE UNMAPPED\n");
  return 0;
}

void waitForDiscard(const void* base) {
  const u32 start = StreamTicks();
  for (;;) {
    const int status = AnonQuery(base);
    if (status == S_CHANGED) { return; }
    require(status == S_OK && StreamTicks() - start < 900, "purge status did not change");
    Yield(1);
  }
}

using Protection = MemoryProtection;
constexpr unsigned FragmentQuotaPages = ANON_MAX_FRAGMENTS + 4;
constexpr unsigned FragmentQuotaSplitPage = ANON_MAX_FRAGMENTS + 1;
int writeFault(void* address) {
  *static_cast<volatile u8*>(address) = 7;
  require(false, "protected write resumed");
  return S_INVALID;
}

void fillFragmentQuota(volatile u8* base) {
  // A two-page head lets range-unmap start inside it and traverse every fragment.
  require(sino::mprotect((void*)base, 2 * PageBytes, Protection::Read) == S_OK,
          "read-only two-page head");
  for (unsigned i = 3; i < ANON_MAX_FRAGMENTS; i += 2) {
    require(sino::mprotect((void*)(base + i * PageBytes), PageBytes, Protection::Read) == S_OK,
            "fill fragment quota with alternating flags");
  }
}

int fragmentTest(const char* mode) {
  const bool budget = streq(mode, "e1-budget");
  volatile u8* page = reserve(budget ? FragmentQuotaPages : 4);
  auto addr = [&](unsigned i) { return (void*)(page + i * PageBytes); };
  auto protect = [&](unsigned i, unsigned n, bool write) {
    return sino::mprotect(addr(i), n * PageBytes, write ? Protection::ReadWrite : Protection::Read);
  };
  auto output = [&](unsigned i) { return QuerySystemInfo((SystemInfo*)addr(i)); };
  if (budget) {
    fillFragmentQuota(page);
    require(AnonMapStatus(AnonMap(1)) == S_LIMIT && protect(FragmentQuotaSplitPage, 1, false) == S_LIMIT &&
            output(FragmentQuotaSplitPage) == S_OK, "fragment quota and protection rollback");
    require(protect(ANON_MAX_FRAGMENTS - 1, 1, true) == S_OK &&
            protect(FragmentQuotaSplitPage, 1, false) == S_OK, "reuse fragment quota");
    require(protect(0, FragmentQuotaPages, true) == S_OK && output(FragmentQuotaSplitPage) == S_OK &&
            AnonSetPurgeable(addr(0), true) == S_OK, "coalesce all fragments at the quota");
  } else if (streq(mode, "e1-oom")) {
    page[PageBytes] = 61;
    require(protect(1, 1, false) == S_LIMIT && page[PageBytes] == 61 && output(1) == S_OK &&
            protect(1, 1, false) == S_OK, "second boundary rollback");
  } else if (streq(mode, "e1-identity")) {
    for (unsigned i = 0; i < 40; ++i) {
      auto* other = reserve(1);
      require(other == page + 4 * PageBytes && protect(3, 2, false) == S_INVALID &&
              output(3) == S_OK && AnonUnmap((void*)other) == S_OK, "independent allocation");
    }
  } else {
    page[PageBytes] = 42;
    require(protect(1, 1, false) == S_OK && output(1) == S_INVALID &&
            page[PageBytes] == 42 && output(2) == S_OK, "resident middle split");
    if (streq(mode, "e1-range")) {
      require(protect(0, 2, false) == S_OK && protect(2, 2, false) == S_OK &&
              output(0) == S_INVALID && output(3) == S_INVALID, "prefix and suffix");
    } else if (streq(mode, "e1-legacy")) {
      require(AnonQuery(addr(1)) == S_INVALID && AnonUnmap(addr(2)) == S_INVALID &&
              AnonLock(addr(1)) == S_INVALID, "reject interior bases");
    } else if (streq(mode, "e1-purge")) {
      require(AnonSetPurgeable(addr(0), true) == S_INVALID && protect(1, 1, true) == S_OK &&
              page[PageBytes] == 42 && AnonSetPurgeable(addr(0), true) == S_OK, "merged RW region");
      require(AnonLock(addr(0)) == S_OK && AnonUnmap(addr(0)) == S_BUSY &&
              AnonUnlock(addr(0)) == S_OK, "merged allocation lock");
    } else {
      require(false, "unknown anonymous fragment probe mode");
    }
  }
  require(AnonUnmap(addr(0)) == S_OK && reserve(4) == page, "whole allocation released");
  return 0;
}

struct RangeRead {
  int handle;
  void* buffer;
  volatile bool started;
};

int readFromPipe(void* argument) {
  auto& read = *static_cast<RangeRead*>(argument);
  read.started = true;
  return StreamReadBlocking(read.handle, read.buffer, 1, 0);
}

enum class ReadMapping : u8 { Hole, ReadOnly, ReadWrite };

void waitAcrossUnmap(ReadMapping mapping) {
  auto* base = reserve(3);
  auto* buffer = base + PageBytes;
  *buffer = 'A';
  PipePair pipe;
  require(PipeCreate(16, &pipe) == S_OK, "create range wait pipe");
  RangeRead read{(int)pipe.read, (void*)buffer, false};
  const int tid = sino::thread::create(readFromPipe, &read);
  require(tid > 0, "start range waiter");
  const u32 start = StreamTicks();
  while (!read.started && StreamTicks() - start < 100) { Yield(1); }
  // Give the reader a dispatch after announcing entry to the empty pipe read.
  Yield(4);
  ThreadResult result;
  require(read.started && sino::thread::query(tid, &result) == S_AGAIN,
          "empty pipe read is pending");
  require(AnonUnmapRange((void*)buffer, PageBytes) == S_OK, "unmap pending read buffer");
  if (mapping != ReadMapping::Hole) {
    require(reserve(1) == buffer, "first fit replaces pending read buffer");
    require(*buffer == 0, "replacement page has fresh backing");
    *buffer = 'B';
    if (mapping == ReadMapping::ReadOnly) {
      require(sino::mprotect((void*)buffer, PageBytes, MemoryProtection::Read) == S_OK,
              "make replacement buffer read-only");
    }
  }
  require(StreamWrite(pipe.write, "R", 1) == 1, "wake pending range reader");
  require(sino::thread::join(tid, &result, 100) == S_OK && result.tid == (u32)tid,
          "range waiter completes normally");
  char queued = 0;
  if (mapping == ReadMapping::ReadWrite) {
    require(result.code == 1 && *buffer == 'R' &&
            StreamRead(pipe.read, &queued, 1) == S_AGAIN, "retry writes to the replacement buffer");
  } else {
    require(result.code == S_INVALID && StreamRead(pipe.read, &queued, 1) == 1 && queued == 'R',
            "invalid output leaves pipe data queued");
    if (mapping == ReadMapping::ReadOnly) {
      require(*buffer == 'B', "read-only replacement retains its content");
    }
  }
  require(Close(pipe.read) == S_OK && Close(pipe.write) == S_OK &&
          AnonUnmap((void*)base) == S_OK, "release range wait fixture");
  if (mapping != ReadMapping::Hole) {
    require(AnonUnmap((void*)buffer) == S_OK, "release replacement buffer");
  }
}

int unmapRangeTest(const char* mode) {
  if (streq(mode, "e2-wait-hole")) {
    waitAcrossUnmap(ReadMapping::Hole);
    return 0;
  }
  if (streq(mode, "e2-wait-remap")) {
    waitAcrossUnmap(ReadMapping::ReadOnly);
    waitAcrossUnmap(ReadMapping::ReadWrite);
    return 0;
  }
  using Protection = MemoryProtection;
  volatile u8* page = reserve(8);
  auto addr = [&](unsigned i) -> void* { return (void*)(page + i * PageBytes); };
  auto unmap = [&](unsigned i, unsigned n) {
    return AnonUnmapRange(addr(i), n * PageBytes);
  };
  const bool shrink = streq(mode, "e2-shrink");
  for (unsigned i = 0; i < 8; ++i) {
    if (shrink && (i == 3 || i == 6)) { continue; }
    page[i * PageBytes] = (u8)(0x10 + i);
  }

  if (shrink) {
    SystemInfo info;
    require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_ANON_UNMAP_RANGE),
            "range-unmap feature bit");
    const u32 tailCapacity = ANON_MAX_BYTES - 6 * PageBytes;
    require(AnonMapStatus(AnonMap(tailCapacity)) == S_LIMIT, "tail still occupied before shrink");
    require(unmap(6, 2) == S_OK, "tail shrink");
    require(page[0] == 0x10 && page[5 * PageBytes] == 0x15, "tail content");
    require(unmap(2, 2) == S_OK, "middle hole");
    require(page[0] == 0x10 && page[PageBytes] == 0x11, "front after hole");
    require(page[4 * PageBytes] == 0x14 && page[5 * PageBytes] == 0x15, "back after hole");
    require(AnonMapStatus(AnonMap(tailCapacity + PageBytes)) == S_LIMIT,
            "disjoint free ranges cannot satisfy a larger contiguous allocation");
    const u32 tail = AnonMap(tailCapacity);
    const u32 middle = AnonMap(2 * PageBytes);
    require(tail == (u32)addr(6) && middle == (u32)addr(2) && AnonMapStatus(AnonMap(PageBytes)) == S_LIMIT,
            "new reservations fill the tail and middle hole exactly");
    require(AnonUnmap((void*)tail) == S_OK && AnonUnmap((void*)middle) == S_OK, "release both independent allocations");
    require(AnonUnmap(addr(0)) == S_OK && reserve(8) == page, "cleanup shrink");
  } else if (streq(mode, "e2-cross")) {
    auto* adjacent = reserve(1);
    require(adjacent == page + 8 * PageBytes &&
            unmap(7, 2) == S_INVALID, "reject adjacent allocation boundary");
    require(sino::mprotect(addr(4), 4 * PageBytes, Protection::Read) == S_OK, "split");
    require(unmap(3, 3) == S_OK, "cross-permission");
    require(page[2 * PageBytes] == 0x12 && page[6 * PageBytes] == 0x16, "cross-perm content");
    require(unmap(1, 6) == S_INVALID, "cross-hole");
    require(sino::mprotect(addr(1), 6 * PageBytes, Protection::Read) == S_INVALID, "protect across hole");
    require(unmap(0, 2) == S_INVALID, "base covered");
    require(AnonUnmapRange((void*)(page + 1), PageBytes) == S_INVALID, "misaligned");
    require(AnonUnmapRange(addr(1), 0) == S_INVALID, "zero bytes");
    require(api(SYS_ANON_UNMAP_RANGE, (u32)addr(1), PageBytes, 1) == S_INVALID, "edx nonzero");
    require(page[0] == 0x10, "atomic reject");
    require(AnonUnmap(addr(0)) == S_OK && AnonUnmap((void*)adjacent) == S_OK, "cleanup cross");

    auto* fragmented = reserve(FragmentQuotaPages);
    auto* split = fragmented + FragmentQuotaSplitPage * PageBytes;
    fragmented[0] = 0x49;
    *split = 0x52;
    fillFragmentQuota(fragmented);
    require(AnonUnmapRange((void*)split, PageBytes) == S_LIMIT && *split == 0x52,
            "range unmap leaves the mapping intact when it would exceed the fragment quota");
    require(AnonUnmapRange((void*)(fragmented + PageBytes), (FragmentQuotaPages - 1) * PageBytes) == S_OK &&
            fragmented[0] == 0x49 && reserve(FragmentQuotaPages - 1) == fragmented + PageBytes,
            "range unmap traverses every fragment and releases the hole");
    require(AnonUnmap((void*)fragmented) == S_OK && AnonUnmap((void*)(fragmented + PageBytes)) == S_OK,
            "release original head and independent hole allocation");
  } else if (streq(mode, "e2-reuse")) {
    require(unmap(3, 2) == S_OK, "punch hole");
    const u32 newBase = AnonMap(2 * PageBytes);
    require(newBase == (u32)addr(3), "reuse exact hole address");
    volatile u8* np = (volatile u8*)newBase;
    np[0] = 0xAA;
    require(AnonUnmap(addr(0)) == S_OK, "free old");
    require(np[0] == 0xAA, "new survives");
    require(AnonUnmap((void*)np) == S_OK, "free new");
  } else if (streq(mode, "e2-oom")) {
    page[3 * PageBytes] = 0x55;
    require(unmap(3, 2) == S_LIMIT, "oom rollback");
    require(page[3 * PageBytes] == 0x55, "content after oom");
    require(AnonMapStatus(AnonMap(ANON_MAX_BYTES - 6 * PageBytes)) == S_LIMIT, "oom leaves reservation charged");
    require(unmap(3, 2) == S_OK, "retry after oom");
    require(AnonUnmap(addr(0)) == S_OK, "cleanup oom");
  } else {
    require(false, "unknown range-unmap probe mode");
  }
  return 0;
}

int protectionTest(const char* mode) {
  volatile u8* page = reserve(1);
  void* base = (void*)page;
  if (streq(mode, "mp-retry-write")) {
    require(sino::thread::create(writeFault, base, ThreadCreateFlags::Detached) > 0,
            "start faulting writer");
    // Stay runnable. The worker's deferred fault queues it behind this thread.
    for (;;) {
      const u32 tmp = AnonMap(1);
      const int status = AnonMapStatus(tmp);
      if (status == S_NOMEM) { break; }
      require(status == S_OK && AnonUnmap((void*)tmp) == S_OK, "wait for injected pressure");
    }
    require(sino::mprotect(base, PageBytes, Protection::Read) == S_OK, "downgrade before retry");
    Log("MP DOWNGRADED\n");
    hold();
  }
  if (streq(mode, "mp-abi")) {
    SystemInfo info;
    require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_MPROTECT) &&
            sino::mprotect((void*)(page + 1), PageBytes, Protection::Read) == S_INVALID &&
            sino::mprotect(base, 1, Protection::Read) == S_INVALID &&
            api(SYS_MPROTECT, (u32)base, PageBytes, 7) == S_INVALID &&
            sino::mprotect((void*)USER_ARGUMENTS, PageBytes, Protection::ReadWrite) == S_INVALID &&
            AnonSetPurgeable(base, true) == S_OK &&
            sino::mprotect(base, PageBytes, Protection::Read) == S_INVALID &&
            AnonSetPurgeable(base, false) == S_OK, "protection ABI and eligibility");
  }
  if (streq(mode, "mp-io")) {
    volatile u8* tail = reserve(1);
    PipePair pair;
    require(tail == page + PageBytes && PipeCreate(16, &pair) == S_OK, "adjacent output fixture");
    void* cross = (void*)(tail - 1);
    require(StreamWrite(pair.write, "AB", 2) == 2 &&
            StreamReadBlocking(pair.read, cross, 2, 100) == 2, "cross-region output");
    require(AnonUnmap(base) == S_OK && AnonUnmap((void*)tail) == S_OK &&
            reserve(1) == page && reserve(1) == tail, "fresh lazy prefix and suffix");
    char data[2];
    require(sino::mprotect((void*)tail, PageBytes, Protection::Read) == S_OK &&
            StreamWrite(pair.write, "CD", 2) == 2 &&
            StreamReadBlocking(pair.read, cross, 2, 100) == S_INVALID &&
            StreamReadBlocking(pair.read, data, 2, 100) == 2 && data[0] == 'C' && data[1] == 'D',
            "read-only suffix preserves queued input");
    return 0;
  }
  const bool hot = streq(mode, "mp-hot");
  const bool cold = streq(mode, "mp-cold");
  const bool retryRead = streq(mode, "mp-retry-read");
  require(hot || cold || retryRead || streq(mode, "mp-abi"), "unknown protection probe mode");
  if (hot) { *page = 42; }
  require(sino::mprotect(base, PageBytes, Protection::Read) == S_OK &&
          AnonSetPurgeable(base, true) == S_INVALID, "protect ordinary region");
  if (cold || hot) {
    Log("MP EXPECT WRITE FAULT\n");
    *page = 77;
    require(false, "protected write succeeded");
  }
  require(*page == 0, "lazy read-only page is zero");
  if (retryRead) {
    Log("MP RETRIED READ\n");
    *page = 77;
    require(false, "retried read granted write");
  }
  require(sino::mprotect(base, PageBytes, Protection::ReadWrite) == S_OK, "restore write");
  *page = 37;
  require(sino::mprotect(base, PageBytes, Protection::Read) == S_OK && *page == 37, "preserve contents");
  return 0;
}

int purgeAbi() {
  SystemInfo info;
  require(QuerySystemInfo(&info) == S_OK && (info.features & FEATURE_PURGEABLE), "purgeable feature bit");
  void* base = (void*)reserve(2);
  const u32 address = (u32)base;
  require(AnonQuery(nullptr) == S_INVALID && AnonQuery((void*)(address + PageBytes)) == S_INVALID &&
          AnonLock((void*)(address + 1)) == S_INVALID && AnonLock(base) == S_INVALID &&
          api(0x158, address, 3, 0) == S_INVALID && api(0x158, address, 1, 1) == S_INVALID &&
          api(0x159, address, 1, 0) == S_INVALID && api(0x15a, address, 0, 1) == S_INVALID,
          "purge calls require an exact base and valid arguments");
  require(AnonQuery(base) == S_OK && AnonSetPurgeable(base, true) == S_OK &&
          AnonSetPurgeable(base, true) == S_OK && AnonUnlock(base) == S_INVALID,
          "a newly marked region starts unlocked");
  void* unaligned = (void*)(address + 1);
  require(AnonQuery(unaligned) == S_INVALID && AnonSetPurgeable(unaligned, false) == S_INVALID &&
          AnonLock(unaligned) == S_INVALID, "marked regions reject unaligned bases");
  require(AnonLock(base) == S_OK && AnonLock(base) == S_OK && AnonUnlock(base) == S_OK &&
          AnonSetPurgeable(base, false) == S_BUSY && AnonUnmap(base) == S_BUSY &&
          AnonUnlock(base) == S_OK && AnonSetPurgeable(base, false) == S_OK &&
          AnonLock(base) == S_INVALID && AnonUnmap(base) == S_OK && AnonQuery(base) == S_INVALID,
          "nested locks pin the region until the last release");

  base = (void*)reserve(1);
  require(AnonSetPurgeable(base, true) == S_OK, "mark lock saturation fixture");
  for (u32 i = 0; i < 0xffffu; ++i) { require(AnonLock(base) == S_OK, "lock count up to maximum"); }
  require(AnonLock(base) == S_LIMIT && AnonUnlock(base) == S_OK && AnonLock(base) == S_OK &&
          AnonLock(base) == S_LIMIT, "overflow leaves lock count intact");
  Log("PURGE ABI PASS\n");
  return 0;
}

int purgeCache() {
  volatile u8* cache = reserve(3);
  volatile u8* pinned = reserve(1);
  volatile u8* ordinary = reserve(1);
  void* empty = (void*)reserve(1);
  void* base = (void*)cache;
  cache[0] = cache[PageBytes] = cache[2 * PageBytes] = 0x6d;
  *pinned = 0xab;
  *ordinary = 0xcd;
  require(AnonSetPurgeable(base, true) == S_OK && AnonSetPurgeable((void*)pinned, true) == S_OK &&
          AnonLock((void*)pinned) == S_OK && AnonSetPurgeable(empty, true) == S_OK,
          "seed one discardable, one pinned and one empty cache");
  Log("PURGE CACHE READY\n");
  waitForDiscard(base);
  require(AnonQuery((void*)pinned) == S_OK && *pinned == 0xab && *ordinary == 0xcd &&
          AnonQuery(empty) == S_OK, "pin, ordinary neighbor and empty reservation survive");
  Log("PURGE CACHE DROPPED\n");
  waitForPressure(false);
  require(AnonSetPurgeable(base, true) == S_BUSY && AnonSetPurgeable(base, false) == S_OK &&
          AnonQuery(base) == S_CHANGED && AnonSetPurgeable(base, true) == S_OK &&
          AnonLock(base) == S_OK && AnonQuery(base) == S_CHANGED, "loss stays visible through mode and lock changes");
  auto* info = (SystemInfo*)(cache + PageBytes);
  require(cache[0] == 0 && cache[2 * PageBytes] == 0 && QuerySystemInfo(info) == S_OK &&
          info->size == sizeof(*info) && cache[2 * PageBytes - 1] == 0 && AnonQuery(base) == S_CHANGED,
          "user touch and syscall preparation repopulate with zeroed pages");
  cache[0] = 0xa4;
  require(AnonSetPurgeable(base, true) == S_OK && AnonQuery(base) == S_OK &&
          AnonUnlock(base) == S_OK && cache[0] == 0xa4 && AnonUnmap(base) == S_OK &&
          AnonUnlock((void*)pinned) == S_OK, "locked rebuild acknowledges the loss");
  Log("PURGE CACHE PASS\n");
  return 0;
}

[[noreturn]] void purgeAddressOrder() {
  void* hole = (void*)reserve(1);
  volatile u8* lower = reserve(1);
  require(AnonUnmap(hole) == S_OK, "open a one-page hole below the first candidate");
  // The larger reservation skips the hole but would reuse its old region slot.
  volatile u8* higher = reserve(2);
  require((u32)higher > (u32)lower, "later candidate is above the first");
  *lower = 0x41;
  *higher = 0x42;
  require(AnonSetPurgeable((void*)lower, true) == S_OK &&
          AnonSetPurgeable((void*)higher, true) == S_OK, "mark two equally resident candidates");
  Log("PURGE ADDRESS ORDER READY\n");
  waitForDiscard((void*)lower);
  require(AnonQuery((void*)higher) == S_OK && *higher == 0x42, "the higher candidate survives the tie");
  Log("PURGE ADDRESS ORDER PASS\n");
  hold();
}

[[noreturn]] void purgeUnlock() {
  volatile u8* cache = reserve(1);
  void* base = (void*)cache;
  *cache = 0x41;
  require(AnonSetPurgeable(base, true) == S_OK && AnonLock(base) == S_OK && AnonLock(base) == S_OK,
          "pin before pressure");
  Log("PURGE UNLOCK READY\n");
  waitForPressure(true);
  require(AnonUnlock(base) == S_OK, "drop the first pin");
  Yield(5);
  require(AnonQuery(base) == S_OK && *cache == 0x41 && AnonUnlock(base) == S_OK,
          "last pin alone makes resident pages eligible");
  waitForDiscard(base);
  require(AnonLock(base) == S_OK && *cache == 0 && AnonQuery(base) == S_CHANGED,
          "repopulate without acknowledging the loss");
  *cache = 0x29;
  require(AnonUnlock(base) == S_OK, "unacknowledged pages remain eligible");
  Log("PURGE UNLOCK PASS\n");
  hold();
}
}

int main() {
  const char* mode = Arguments();
  CliInfo cli;
  char* argv[2];
  if (!*mode && CliArguments(&cli, argv, 2) == 1) { mode = argv[0]; }

  if (mode[0] == 'e' && mode[1] == '1') { return fragmentTest(mode); }
  if (mode[0] == 'e' && mode[1] == '2') { return unmapRangeTest(mode); }
  if (mode[0] == 'm' && mode[1] == 'p' && mode[2] == '-') { return protectionTest(mode); }
  if (streq(mode, "pool-populate") || streq(mode, "pool-denied")) {
    return poolPopulate(streq(mode, "pool-denied"));
  }
  if (streq(mode, "purge-abi")) { return purgeAbi(); }
  if (streq(mode, "purge-cache")) { return purgeCache(); }
  if (streq(mode, "purge-address-order")) { purgeAddressOrder(); }
  if (streq(mode, "purge-unlock")) { purgeUnlock(); }
  if (streq(mode, "purge-hold") || streq(mode, "purge-hold-large")) {
    const u32 pages = streq(mode, "purge-hold") ? 2 : 4;
    volatile u8* memory = reserve(pages);
    for (u32 i = 0; i < pages; ++i) { memory[i * PageBytes] = 1; }
    require(AnonSetPurgeable((void*)memory, true) == S_OK, "mark resident cache");
    Log("PURGE HOLD READY\n");
    hold();
  }

  if (streq(mode, "group-id")) { return queryGroup(); }
  if (streq(mode, "group-hold")) {
    volatile u8* memory = reserve(2);
    memory[0] = memory[PageBytes] = 1;
    logGroup("RECLAIM GROUP MEMBER ", queryGroup());
    hold();
  }
  if (streq(mode, "group-parent") || streq(mode, "group-parent-max")) {
    if (streq(mode, "group-parent-max")) {
      require(SetReclaimPriority(350) == S_OK, "raise parent score before managed run");
    }
    const int group = queryGroup();
    const int shortChild = startGroupChild("group-id");
    ProcessCompletion result;
    require(ProcessWaitFor(shortChild, &result, 500) == S_OK && result.pid != (u32)group &&
            result.reason == PROCESS_EXITED && result.code == group,
            "managed child inherits group and publishes its own completion");
    require(Close(shortChild) == S_OK, "close completed child handle");
    volatile u8* memory = reserve(1);
    *memory = 1;
    startGroupChild("group-hold");
    logGroup("RECLAIM GROUP PARENT ", group);
    hold();
  }

  if (streq(mode, "low")) {
    volatile u8* reserved = reserve(2);
    Log("RECLAIM LOW READY\n");
    const u32 start = (u32)api(SYS_TICKS, 0, 0, 0);
    for (;;) {
      const u32 base = AnonMap(1);
      const int status = AnonMapStatus(base);
      if (status == S_NOMEM) { break; }
      require(status == S_OK && AnonUnmap((void*)base) == S_OK, "poll reservation");
      require((u32)api(SYS_TICKS, 0, 0, 0) - start < 900, "LOW did not arrive");
      Yield(1);
    }
    require(AnonMapStatus(AnonMap(0)) == S_INVALID && AnonMapStatus(AnonMap(ANON_MAX_BYTES)) == S_LIMIT,
            "invalid request and quota retain precedence");
    require(reserved[0] == 0 && reserved[PageBytes] == 0, "existing region can still populate");
    reserved[PageBytes] = 0x6d;
    require(reserved[PageBytes] == 0x6d && AnonUnmap((void*)reserved) == S_OK,
            "existing region remains writable and can be unmapped");
    Log("RECLAIM LOW PASS\n");
    return 0;
  }

  if (streq(mode, "retry") || streq(mode, "fault") || streq(mode, "table-fault")) {
    volatile u8* memory = streq(mode, "table-fault") ? reserveInNewPageTable() : reserve(1);
    Log("RECLAIM TOUCH READY\n");
    require(*memory == 0, "first touch receives a zero page");
    *memory = 0x6d;
    require(*memory == 0x6d && streq(mode, "retry"), "only retry mode may resume");
    Log("RECLAIM TOUCH PASS\n");
    hold();
  }

  if (streq(mode, "copy") || streq(mode, "table-copy")) {
    volatile u8* memory = streq(mode, "table-copy") ? reserveInNewPageTable() : reserve(1);
    require(QuerySystemInfo((SystemInfo*)memory) == S_NOMEM, "system call preserves S_NOMEM");
    Log(streq(mode, "copy") ? "RECLAIM COPY PASS\n" : "RECLAIM TABLE COPY PASS\n");
    hold();
  }

  u32 pages = 0;
  if (streq(mode, "hold-small")) { pages = 2; }
  if (streq(mode, "hold-large")) { pages = 16; }
  if (streq(mode, "protect")) { pages = 24; }
  require(pages > 0, "unknown reclaim probe mode");
  volatile u8* memory = reserve(pages);
  for (u32 i = 0; i < pages; ++i) { memory[i * PageBytes] = 1; }
  Log("RECLAIM HOLD READY\n");
  hold();
}
