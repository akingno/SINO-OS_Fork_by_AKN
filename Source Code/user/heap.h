#ifndef SINO_USER_HEAP_H
#define SINO_USER_HEAP_H

#include "../src32/api.h"
#include <new> // Placement construction in mapper-owned storage.

namespace sino::heap {

struct MapResult {
  void* base;
  int status;
};

// map rounds to pages and returns a page-aligned base on success.
// Failed unmaps leave the mapping intact. fatalUnmap reports a heap invariant
// failure and terminates the process.
struct MappingBackend {
  void* context;
  MapResult (*map)(void* context, u32 bytes);
  int (*unmap)(void* context, void* base);
  int (*setPurgeable)(void* context, void* base, bool enabled);
  void (*fatalUnmap)(void* context, void* base, int status);
};

namespace detail {

using Address = __UINTPTR_TYPE__;
constexpr u32 Alignment = 16;
constexpr u32 SlabBytes = 64 * 1024;
constexpr u32 SlabHeaderBytes = 64;
constexpr u32 ChunkSizes[] = {16, 32, 64, 128, 256, 496, 1008, 2032, 4080};
constexpr u32 ClassCount = sizeof(ChunkSizes) / sizeof(ChunkSizes[0]);
constexpr u32 NoChunk = ~0u;
constexpr u32 HotLimit = 4;
constexpr u32 ColdLimit = 12;

inline void fillBytes(void* start, u32 bytes, u8 value) {
  auto* cursor = static_cast<u8*>(start);
  for (u32 i = 0; i < bytes; ++i) {
    cursor[i] = value;
  }
}

struct FreeEntry {
  u32 next;
};

struct ChunkedBlock {
  ChunkedBlock* previous = nullptr;
  ChunkedBlock* next = nullptr;
  u32 sizeClass;
  u32 payloadOffset;
  u32 capacity;
  u32 freeCount;
  u32 nextUnused = 0;
  u32 firstFree = NoChunk;

  static ChunkedBlock* create(void* base, u32 sizeClass) {
    const u32 size = ChunkSizes[sizeClass];
    u32 count = (SlabBytes - SlabHeaderBytes) / size;
    u32 offset = slotOffset(count);
    while (offset + count * size > SlabBytes) {
      offset = slotOffset(--count);
    }
    auto* block = ::new (base) ChunkedBlock{nullptr, nullptr, sizeClass, offset, count, count};
    fillBytes(block->bitmap(), offset - SlabHeaderBytes, 0);
    return block;
  }

  u32 chunkSize() const { return ChunkSizes[sizeClass]; }

  u32 liveIndex(const void* pointer) const {
    const u32 offset = static_cast<u32>(reinterpret_cast<Address>(pointer) -
                                        reinterpret_cast<Address>(this));
    if (offset < payloadOffset) { return NoChunk; }
    const u32 relative = offset - payloadOffset;
    const u32 size = chunkSize();
    const u32 index = relative / size;
    if (relative % size || index >= nextUnused || !(bitmap()[index / 8] & bit(index))) {
      return NoChunk;
    }
    return index;
  }

  bool isLastLiveChunk() const {
    return freeCount + 1 == capacity;
  }

  void* takeChunk() {
    u32 index;
    if (firstFree != NoChunk) {
      index = firstFree;
      firstFree = static_cast<FreeEntry*>(chunk(index))->next;
    } else {
      index = nextUnused++;
    }
    bitmap()[index / 8] |= bit(index);
    --freeCount;
    void* pointer = chunk(index);
#ifdef HEAP_DEBUG
    fillBytes(pointer, chunkSize(), 0xDC);
#endif
    return pointer;
  }

  void putChunk(u32 index) {
    void* pointer = chunk(index);
#ifdef HEAP_DEBUG
    fillBytes(pointer, chunkSize(), 0xED);
#endif
    ::new (pointer) FreeEntry{firstFree};
    firstFree = index;
    bitmap()[index / 8] &= static_cast<u8>(~bit(index));
    ++freeCount;
  }

private:
  static u32 slotOffset(u32 slots) {
    const u32 bitmapBytes = (slots + 7) / 8;
    return SlabHeaderBytes + ((bitmapBytes + Alignment - 1) & ~(Alignment - 1));
  }

  static u8 bit(u32 index) { return static_cast<u8>(1u << (index % 8)); }

  const u8* bitmap() const { return reinterpret_cast<const u8*>(this) + SlabHeaderBytes; }
  u8* bitmap() { return reinterpret_cast<u8*>(this) + SlabHeaderBytes; }
  void* chunk(u32 index) {
    return reinterpret_cast<u8*>(this) + payloadOffset + index * chunkSize();
  }
};

struct alignas(Alignment) BigBlock {
  static constexpr u32 Magic = 0x48424947u;
  u32 magic;
  u32 bytes;
};

static_assert(sizeof(ChunkedBlock) <= SlabHeaderBytes, "slab header fits its reserved prefix");
static_assert(sizeof(BigBlock) == Alignment, "large payload retains heap alignment");
static_assert(sizeof(FreeEntry) <= ChunkSizes[0], "smallest slot holds a free-list entry");

struct BlockList {
  ChunkedBlock* head = nullptr;

  void prepend(ChunkedBlock& block) {
    block.previous = nullptr;
    block.next = head;
    if (head) { head->previous = &block; }
    head = &block;
  }

  void remove(ChunkedBlock& block) {
    if (block.previous) {
      block.previous->next = block.next;
    } else {
      head = block.next;
    }
    if (block.next) { block.next->previous = block.previous; }
    block.previous = nullptr;
    block.next = nullptr;
  }
};

struct Allocator {
  BlockList usable;
  BlockList full;
};

enum class BlockKind : u8 { Active, Hot, Cold, Direct };

struct RegistryEntry {
  void* base = nullptr;
  BlockKind kind = BlockKind::Active;
  u8 sizeClass = 0;
};

struct BlockRegistry {
  RegistryEntry entries[ANON_MAX_REGIONS]{};

  RegistryEntry* vacant() {
    for (auto& entry : entries) {
      if (!entry.base) { return &entry; }
    }
    return nullptr;
  }

  const RegistryEntry* find(const void* pointer) const {
    const auto address = reinterpret_cast<Address>(pointer);
    for (const auto& entry : entries) {
      if (!entry.base) { continue; }
      const auto base = reinterpret_cast<Address>(entry.base);
      if (address < base) { continue; }
      const auto offset = address - base;
      if (entry.kind == BlockKind::Direct ? offset == sizeof(BigBlock) : offset < SlabBytes) {
        return &entry;
      }
    }
    return nullptr;
  }

  RegistryEntry* find(const void* pointer) {
    return const_cast<RegistryEntry*>(static_cast<const BlockRegistry&>(*this).find(pointer));
  }
};

// Oldest entry first. All links remain valid even if a cold slab is purged.
template<u32 Limit>
struct SlabCache {
  RegistryEntry* entries[Limit]{};
  u32 count = 0;

  bool full() const { return count == Limit; }

  void append(RegistryEntry& entry) { entries[count++] = &entry; }

  void remove(u32 index) {
    for (u32 i = index + 1; i < count; ++i) { entries[i - 1] = entries[i]; }
    entries[--count] = nullptr;
  }
};

} // namespace detail

// One instance owns a set of mappings. Callers serialize access to the instance.
class Heap {
public:
  constexpr explicit Heap(MappingBackend backend) : backend_(backend) {}
  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  void* allocate(u32 bytes) {
    if (!bytes || bytes > ANON_MAX_BYTES - sizeof(detail::BigBlock)) { return nullptr; }
    for (u32 sizeClass = 0; sizeClass < detail::ClassCount; ++sizeClass) {
      if (bytes <= detail::ChunkSizes[sizeClass]) { return allocateSmall(sizeClass); }
    }
    auto* entry = reserveMapping(bytes + sizeof(detail::BigBlock), detail::BlockKind::Direct);
    if (!entry) { return nullptr; }
    ::new (entry->base) detail::BigBlock{detail::BigBlock::Magic, bytes};
    return static_cast<u8*>(entry->base) + sizeof(detail::BigBlock);
  }

  void* allocateZeroed(u32 count, u32 bytes) {
    if (!bytes || count > (~0u) / bytes) { return nullptr; }
    const u32 total = count * bytes;
    void* pointer = allocate(total);
    if (pointer) { detail::fillBytes(pointer, total, 0); }
    return pointer;
  }

  u32 usableSize(const void* pointer) const {
    const auto* entry = registry_.find(pointer);
    if (!entry) { return 0; }
    if (entry->kind == detail::BlockKind::Direct) {
      const auto& header = *static_cast<const detail::BigBlock*>(entry->base);
      return header.magic == detail::BigBlock::Magic ? header.bytes : 0;
    }
    if (entry->kind != detail::BlockKind::Active) { return 0; }
    const auto& slab = *static_cast<const detail::ChunkedBlock*>(entry->base);
    return slab.liveIndex(pointer) != detail::NoChunk ? slab.chunkSize() : 0;
  }

  void* reallocate(void* pointer, u32 bytes) {
    if (!pointer) { return allocate(bytes); }
    if (!bytes) {
      release(pointer);
      return nullptr;
    }
    const u32 capacity = usableSize(pointer);
    if (!capacity) { return nullptr; }
    if (bytes <= capacity) { return pointer; }
    auto* destination = static_cast<u8*>(allocate(bytes));
    if (!destination) { return nullptr; }
    const auto* source = static_cast<const u8*>(pointer);
    for (u32 i = 0; i < capacity; ++i) { destination[i] = source[i]; }
    release(pointer);
    return destination;
  }

  // Invalid pointers are rejected before reading cached or unmapped storage.
  bool release(void* pointer) {
    if (!pointer) { return true; }
    auto* entry = registry_.find(pointer);
    if (!entry) { return false; }
    if (entry->kind == detail::BlockKind::Direct) {
      const auto* header = static_cast<const detail::BigBlock*>(entry->base);
      if (header->magic != detail::BigBlock::Magic) { return false; }
      unmapBlock(*entry);
      return true;
    }
    if (entry->kind != detail::BlockKind::Active) { return false; }

    auto& block = *static_cast<detail::ChunkedBlock*>(entry->base);
    const u32 index = block.liveIndex(pointer);
    if (index == detail::NoChunk) { return false; }
    auto& allocator = allocators_[block.sizeClass];
    if (block.isLastLiveChunk()) {
      const u32 sameClass = findHot(block.sizeClass);
      if (sameClass < hot_.count) {
        demoteHot(sameClass);
      } else if (hot_.full()) {
        demoteHot(0);
      }
      auto& list = block.freeCount ? allocator.usable : allocator.full;
      list.remove(block);
      block.putChunk(index);
      entry->kind = detail::BlockKind::Hot;
      hot_.append(*entry);
      return true;
    }
    if (!block.freeCount) {
      allocator.full.remove(block);
      allocator.usable.prepend(block);
    }
    block.putChunk(index);
    return true;
  }

private:
  void* allocateSmall(u32 sizeClass) {
    auto& allocator = allocators_[sizeClass];
    auto* block = allocator.usable.head;
    if (!block) {
      block = activateSlab(sizeClass);
      if (!block) { return nullptr; }
      allocator.usable.prepend(*block);
    }
    void* pointer = block->takeChunk();
    if (!block->freeCount) {
      allocator.usable.remove(*block);
      allocator.full.prepend(*block);
    }
    return pointer;
  }

  u32 findHot(u32 sizeClass) const {
    for (u32 i = 0; i < hot_.count; ++i) {
      if (hot_.entries[i]->sizeClass == sizeClass) { return i; }
    }
    return hot_.count;
  }

  detail::ChunkedBlock* activateSlab(u32 sizeClass) {
    const u32 hotIndex = findHot(sizeClass);
    if (hotIndex < hot_.count) {
      auto& entry = *hot_.entries[hotIndex];
      hot_.remove(hotIndex);
      entry.kind = detail::BlockKind::Active;
      return static_cast<detail::ChunkedBlock*>(entry.base);
    }
    while (cold_.count) {
      // The previous in-slab header may have been discarded by the kernel.
      const u32 index = cold_.count - 1;
      auto& entry = *cold_.entries[index];
      if (backend_.setPurgeable(backend_.context, entry.base, false) == S_OK) {
        cold_.remove(index);
        entry.kind = detail::BlockKind::Active;
        entry.sizeClass = static_cast<u8>(sizeClass);
        return detail::ChunkedBlock::create(entry.base, sizeClass);
      }
      evict(cold_, index);
    }
    auto* entry = reserveMapping(detail::SlabBytes, detail::BlockKind::Active);
    if (!entry) { return nullptr; }
    entry->sizeClass = static_cast<u8>(sizeClass);
    return detail::ChunkedBlock::create(entry->base, sizeClass);
  }

  void demoteHot(u32 index) {
    auto& entry = *hot_.entries[index];
    if (backend_.setPurgeable(backend_.context, entry.base, true) != S_OK) {
      evict(hot_, index);
      return;
    }
    if (cold_.full()) { evict(cold_, 0); }
    hot_.remove(index);
    entry.kind = detail::BlockKind::Cold;
    cold_.append(entry);
  }

  template<u32 Limit>
  void evict(detail::SlabCache<Limit>& cache, u32 index) {
    unmapBlock(*cache.entries[index]);
    cache.remove(index);
  }

  bool evictOldest() {
    if (cold_.count) {
      evict(cold_, 0);
    } else if (hot_.count) {
      evict(hot_, 0);
    } else {
      return false;
    }
    return true;
  }

  detail::RegistryEntry* reserveMapping(u32 bytes, detail::BlockKind kind) {
    for (u32 retry = 0; ; ++retry) {
      if (auto* entry = registry_.vacant()) {
        const MapResult result = backend_.map(backend_.context, bytes);
        if (result.status == S_OK && result.base) {
          *entry = {result.base, kind};
          return entry;
        }
        if (result.status != S_LIMIT && result.status != S_NOMEM) { return nullptr; }
      }
      if (retry == 2 || !evictOldest()) { return nullptr; }
      // Free one cached reservation for the common case, then drain once.
      if (retry == 1) {
        while (evictOldest()) {}
      }
    }
  }

  void unmapBlock(detail::RegistryEntry& entry) {
    const int status = backend_.unmap(backend_.context, entry.base);
    if (status != S_OK) {
      backend_.fatalUnmap(backend_.context, entry.base, status);
      __builtin_trap();
    }
    entry = {};
  }

  MappingBackend backend_;
  detail::Allocator allocators_[detail::ClassCount]{};
  detail::BlockRegistry registry_{};
  detail::SlabCache<detail::HotLimit> hot_{};
  detail::SlabCache<detail::ColdLimit> cold_{};
};

} // namespace sino::heap

#endif
