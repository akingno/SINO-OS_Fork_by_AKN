#ifndef SINO_MM_VMA_H
#define SINO_MM_VMA_H

#include "../types.h"

namespace mm {

enum class VmaType : u8 { Anonymous, Stack, Image, Params, Probe, Guard };

struct VmaNode {
  enum class Flag : u8 { Read = 1, Write = 2, Execute = 4, Purgeable = 8, Purged = 16, Head = 32 };

  u32 startPage;
  u32 pageCount;
  u32 resident;
  u16 lock_count;
  u8 flags;
  VmaType type;
  i8 balance;
  bool linked;
  u8 allocId;
  VmaNode* left;
  VmaNode* right;
  VmaNode* parent;

  bool hasFlag(Flag flag) const { return flags & static_cast<u8>(flag); }
  void setFlag(Flag flag, bool enabled) {
    const u8 bit = static_cast<u8>(flag);
    flags = enabled ? flags | bit : flags & ~bit;
  }
};
static_assert(sizeof(void*) != 4 || sizeof(VmaNode) == 32, "VMA node must fit the i386 32-byte slab class");

// Insert a zero-initialized or previously detached node with a valid range.
// Intervals are half-open in page numbers. Keep the range fixed while linked.
bool vma_insert(VmaNode*& root, VmaNode* node);
// Detach this exact node without freeing it or moving any other node's payload.
bool vma_remove(VmaNode*& root, VmaNode* node);

VmaNode* vma_find(VmaNode* root, u32 page);
const VmaNode* vma_find(const VmaNode* root, u32 page);
VmaNode* vma_find_exact(VmaNode* root, u32 startPage);
VmaNode* vma_lower_bound(VmaNode* root, u32 page);
// Walk in ascending address order while the tree is unchanged.
VmaNode* vma_first(VmaNode* root);
VmaNode* vma_next(VmaNode* node);
const VmaNode* vma_next(const VmaNode* node);
// Find the lowest free range of pageCount pages within [beginPage, endPage).
bool vma_find_gap(VmaNode* root, u32 beginPage, u32 endPage, u32 pageCount, u32& startPage);

// Every linked node must have been allocated by kmalloc. Clears root.
u32 vma_destroy(VmaNode*& root);
u32 vma_count(const VmaNode* root);
bool vma_check(const VmaNode* root);
void vma_selftest();

}

#endif
