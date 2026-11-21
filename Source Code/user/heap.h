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
// Failed unmaps leave the mapping intact.
struct MappingBackend {
  void* context;
  MapResult (*map)(void* context, u32 bytes);
  int (*unmap)(void* context, void* base);
};

namespace detail {

using Address = __UINTPTR_TYPE__;
constexpr u32 Alignment = 16;
constexpr u32 SlabBytes = 64 * 1024;
constexpr u32 SlabHeaderBytes = 64;
constexpr u32 ChunkSizes[] = {16, 32, 64, 128, 256, 496, 1008, 2032, 4080};
constexpr u32 ClassCount = sizeof(ChunkSizes) / sizeof(ChunkSizes[0]);
constexpr u32 NoChunk = ~0u;

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

enum class BlockKind : u8 { Slab, Direct };

struct RegistryEntry {
  void* base = nullptr;
  BlockKind kind = BlockKind::Slab;
};

struct BlockRegistry {
  RegistryEntry entries[ANON_MAX_REGIONS]{};

  RegistryEntry* vacant() {
    for (auto& entry : entries) {
      if (!entry.base) { return &entry; }
    }
    return nullptr;
  }

  RegistryEntry* find(const void* pointer) {
    const auto address = reinterpret_cast<Address>(pointer);
    for (auto& entry : entries) {
      if (!entry.base) { continue; }
      const auto base = reinterpret_cast<Address>(entry.base);
      if (address < base) { continue; }
      const auto offset = address - base;
      if (entry.kind == BlockKind::Slab ? offset < SlabBytes : offset == sizeof(BigBlock)) {
        return &entry;
      }
    }
    return nullptr;
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
    void* base = mapBlock(bytes + sizeof(detail::BigBlock), detail::BlockKind::Direct);
    if (!base) { return nullptr; }
    ::new (base) detail::BigBlock{detail::BigBlock::Magic, bytes};
    return static_cast<u8*>(base) + sizeof(detail::BigBlock);
  }

  void* allocateZeroed(u32 count, u32 bytes) {
    if (!bytes || count > (~0u) / bytes) { return nullptr; }
    const u32 total = count * bytes;
    void* pointer = allocate(total);
    if (pointer) { detail::fillBytes(pointer, total, 0); }
    return pointer;
  }

  // Failure leaves any live allocation and its contents available to the caller.
  bool release(void* pointer) {
    if (!pointer) { return true; }
    auto* entry = registry_.find(pointer);
    if (!entry) { return false; }
    if (entry->kind == detail::BlockKind::Direct) {
      const auto* header = static_cast<const detail::BigBlock*>(entry->base);
      return header->magic == detail::BigBlock::Magic && unmapBlock(*entry);
    }

    auto& block = *static_cast<detail::ChunkedBlock*>(entry->base);
    const u32 index = block.liveIndex(pointer);
    if (index == detail::NoChunk) { return false; }
    auto& allocator = allocators_[block.sizeClass];
    auto& list = block.freeCount ? allocator.usable : allocator.full;
    if (block.isLastLiveChunk()) {
      list.remove(block);
      if (unmapBlock(*entry)) { return true; }
      list.prepend(block);
      return false;
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
      void* base = mapBlock(detail::SlabBytes, detail::BlockKind::Slab);
      if (!base) { return nullptr; }
      block = detail::ChunkedBlock::create(base, sizeClass);
      allocator.usable.prepend(*block);
    }
    void* pointer = block->takeChunk();
    if (!block->freeCount) {
      allocator.usable.remove(*block);
      allocator.full.prepend(*block);
    }
    return pointer;
  }

  void* mapBlock(u32 bytes, detail::BlockKind kind) {
    auto* entry = registry_.vacant();
    if (!entry) { return nullptr; }
    const MapResult result = backend_.map(backend_.context, bytes);
    if (result.status != S_OK || !result.base) { return nullptr; }
    *entry = {result.base, kind};
    return result.base;
  }

  bool unmapBlock(detail::RegistryEntry& entry) {
    if (backend_.unmap(backend_.context, entry.base) != S_OK) { return false; }
    entry = {};
    return true;
  }

  MappingBackend backend_;
  detail::Allocator allocators_[detail::ClassCount]{};
  detail::BlockRegistry registry_{};
};

} // namespace sino::heap

#endif
