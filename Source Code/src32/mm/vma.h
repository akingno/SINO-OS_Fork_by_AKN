#ifndef SINO_MM_VMA_H
#define SINO_MM_VMA_H

#include "../types.h"

namespace mm {

enum class VmaType : u8 { Anonymous, Stack, Code };

struct VmaNode {
  u32 startPage;
  u32 pageCount;
  u16 resident;
  u16 lock_count;
  u8 flags;  // Read, write, execute, purgeable and purged, in that order.
  VmaType type;
  i8 balance;
  bool linked;
  VmaNode* left;
  VmaNode* right;
  VmaNode* parent;
};
static_assert(sizeof(VmaNode) == 28, "VMA node must fit the 32-byte slab class");

// Insert a zero-initialized or previously detached node with a valid range.
// Intervals are half-open in page numbers. Keep the range fixed while linked.
bool vma_insert(VmaNode*& root, VmaNode* node);
// Detach this exact node without freeing it or moving any other node's payload.
bool vma_remove(VmaNode*& root, VmaNode* node);

VmaNode* vma_find(VmaNode* root, u32 page);
VmaNode* vma_find_exact(VmaNode* root, u32 startPage);
VmaNode* vma_lower_bound(VmaNode* root, u32 page);

// Every linked node must have been allocated by kmalloc. Clears root.
u32 vma_destroy(VmaNode*& root);
u32 vma_count(const VmaNode* root);
bool vma_check(const VmaNode* root);
void vma_selftest();

}

#endif
