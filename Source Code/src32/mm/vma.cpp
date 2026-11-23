#include "vma.h"
#include "addr.h"
#include "kmalloc.h"
#include "page.h"
#include "../klog.h"
#include "../panic.h"

namespace mm {
namespace {

constexpr u32 PageLimit = 1u << (32 - PAGE_SHIFT);
// An AVL tree at least doubles its minimum node count every two levels.
constexpr u32 CheckDepth = 2 * (32 - PAGE_SHIFT);

void replace_child(VmaNode*& root, VmaNode* node, VmaNode* replacement) {
  VmaNode* parent = node->parent;
  if (!parent) {
    root = replacement;
  } else if (parent->left == node) {
    parent->left = replacement;
  } else {
    parent->right = replacement;
  }
  if (replacement) {
    replacement->parent = parent;
  }
}

// Balance is right height minus left height, including the transient +/-2.
VmaNode* rotate_left(VmaNode*& root, VmaNode* node) {
  VmaNode* pivot = node->right;
  replace_child(root, node, pivot);
  node->right = pivot->left;
  if (node->right) {
    node->right->parent = node;
  }
  pivot->left = node;
  node->parent = pivot;
  node->balance -= 1 + (pivot->balance > 0 ? pivot->balance : 0);
  pivot->balance += (node->balance < 0 ? node->balance : 0) - 1;
  return pivot;
}

VmaNode* rotate_right(VmaNode*& root, VmaNode* node) {
  VmaNode* pivot = node->left;
  replace_child(root, node, pivot);
  node->left = pivot->right;
  if (node->left) {
    node->left->parent = node;
  }
  pivot->right = node;
  node->parent = pivot;
  node->balance += 1 - (pivot->balance < 0 ? pivot->balance : 0);
  pivot->balance += 1 + (node->balance > 0 ? node->balance : 0);
  return pivot;
}

VmaNode* rebalance(VmaNode*& root, VmaNode* node) {
  if (node->balance == 2) {
    if (node->right->balance < 0) {
      rotate_right(root, node->right);
    }
    return rotate_left(root, node);
  }
  if (node->left->balance > 0) {
    rotate_left(root, node->left);
  }
  return rotate_right(root, node);
}

template <typename Node>
Node* first_node(Node* node) {
  while (node && node->left) {
    node = node->left;
  }
  return node;
}

template <typename Node>
Node* next_node(Node* node) {
  if (node->right) {
    return first_node(node->right);
  }
  while (node->parent && node == node->parent->right) {
    node = node->parent;
  }
  return node->parent;
}

int checked_height(const VmaNode* node, const VmaNode* parent, u32 first, u32 end, u32 depth) {
  if (!node) {
    return 0;
  }
  if (!depth || !node->linked || node->parent != parent || !node->pageCount ||
      node->startPage < first || node->startPage >= end || node->pageCount > end - node->startPage) {
    return -1;
  }
  const int left = checked_height(node->left, node, first, node->startPage, depth - 1);
  const int right = checked_height(node->right, node, node->startPage + node->pageCount, end, depth - 1);
  const int balance = right - left;
  if (left < 0 || right < 0 || balance < -1 || balance > 1 || balance != node->balance) {
    return -1;
  }
  return 1 + (left > right ? left : right);
}

void check_intervals() {
  VmaNode first{100, 10}, second{110, 10}, top{PageLimit - 1, 1};
  VmaNode overlapLeft{95, 6}, overlapRight{119, 2}, empty{120, 0}, pastEnd{PageLimit - 1, 2};
  VmaNode* root = nullptr;
  VmaNode* other = nullptr;
  if (!vma_insert(root, &first) || vma_insert(other, &first) ||
      !vma_insert(root, &second) || !vma_insert(root, &top) ||
      vma_insert(root, &overlapLeft) || vma_insert(root, &overlapRight) ||
      vma_insert(root, &empty) || vma_insert(root, &pastEnd) ||
      vma_remove(root, &overlapLeft) || vma_remove(other, &first) ||
      !vma_check(root) || vma_count(root) != 3) {
    panic("VMA interval insert", 0, 0);
  }
  if (vma_find(root, 109) != &first || vma_find(root, 110) != &second ||
      vma_find(root, 120) || vma_find(root, PageLimit - 1) != &top || vma_find(root, PageLimit) ||
      vma_find_exact(root, 110) != &second || vma_find_exact(root, 111) ||
      vma_lower_bound(root, 99) != &first || vma_lower_bound(root, 110) != &second ||
      vma_lower_bound(root, 111) != &top || vma_lower_bound(root, PageLimit)) {
    panic("VMA boundary lookup", 0, 0);
  }
  u32 startPage;
  if (!vma_find_gap(nullptr, 0, 1, 1, startPage) || startPage != 0 ||
      !vma_find_gap(root, 98, 122, 2, startPage) || startPage != 98 ||
      !vma_find_gap(root, 98, 123, 3, startPage) || startPage != 120 ||
      !vma_find_gap(root, 105, 122, 2, startPage) || startPage != 120 ||
      vma_find_gap(root, 105, 121, 2, startPage) ||
      vma_find_gap(root, PageLimit - 1, PageLimit, 1, startPage) ||
      vma_find_gap(root, 121, 122, 0, startPage) ||
      vma_find_gap(root, 122, 121, 1, startPage) ||
      vma_find_gap(root, PageLimit, PageLimit + 1, 1, startPage)) {
    panic("VMA first-fit boundaries", 0, 0);
  }
  // The root has two children and its successor is its immediate right child.
  if (root != &second || !vma_remove(root, &second) || root != &top || !vma_check(root) ||
      vma_find_exact(root, 100) != &first || vma_find_exact(root, 110) ||
      !vma_insert(root, &second) || !vma_check(root) || !vma_remove(root, &first) ||
      !vma_remove(root, &top) || !vma_remove(root, &second) || root) {
    panic("VMA node identity", 0, 0);
  }
}

void check_mutations(bool shuffled) {
  constexpr u32 Count = 128;  // More nodes than one 32-byte slab page can hold.
  constexpr u32 First = 100;
  VmaNode* nodes[Count]{};
  VmaNode* root = nullptr;
  const u32 before = free_pages;
  for (u32 i = 0; i < Count; ++i) {
    const u32 slot = shuffled ? (i * 53 + 17) % Count : i;
    auto* node = static_cast<VmaNode*>(kmalloc(sizeof(VmaNode)));
    if (!node) {
      panic("VMA self-check allocation", i, Count);
    }
    *node = VmaNode{First + slot * 4, 3};
    nodes[slot] = node;
    if (!vma_insert(root, node) || !vma_check(root)) {
      panic("VMA self-check insert", i, slot);
    }
  }
  for (u32 i = 0; i < Count; ++i) {
    const u32 start = First + i * 4;
    if (vma_find_exact(root, start) != nodes[i] || vma_find(root, start) != nodes[i] ||
        vma_find(root, start + 1) != nodes[i] || vma_find(root, start + 3)) {
      panic("VMA self-check lookup", i, start);
    }
  }
  for (u32 i = 0; i < Count / 2; ++i) {
    const u32 slot = (i * 73 + 63) % Count;
    VmaNode* node = nodes[slot];
    if (!vma_remove(root, node) || node->linked || node->parent || node->left || node->right ||
        !vma_check(root) || vma_count(root) != Count - i - 1) {
      panic("VMA self-check remove", i, slot);
    }
    kfree(node);
    nodes[slot] = nullptr;
    for (u32 j = 0; j < Count; ++j) {
      if (vma_find_exact(root, First + j * 4) != nodes[j]) {
        panic("VMA self-check identity", slot, j);
      }
    }
  }
  if (vma_destroy(root) != Count / 2 || root || free_pages != before) {
    panic("VMA self-check teardown", free_pages, before);
  }
}

}  // namespace

bool vma_insert(VmaNode*& root, VmaNode* node) {
  if (!node || node->linked || node->left || node->right || node->parent || !node->pageCount ||
      node->startPage >= PageLimit || node->pageCount > PageLimit - node->startPage) {
    return false;
  }
  const u32 end = node->startPage + node->pageCount;
  VmaNode* parent = nullptr;
  VmaNode** link = &root;
  while (*link) {
    parent = *link;
    if (end <= parent->startPage) {
      link = &parent->left;
    } else if (node->startPage >= parent->startPage + parent->pageCount) {
      link = &parent->right;
    } else {
      return false;
    }
  }
  *link = node;
  node->parent = parent;
  node->balance = 0;
  node->linked = true;
  for (VmaNode* child = node; parent; child = parent, parent = parent->parent) {
    parent->balance += parent->left == child ? -1 : 1;
    if (!parent->balance) {
      break;
    }
    if (parent->balance < -1 || parent->balance > 1) {
      rebalance(root, parent);
      break;
    }
  }
  return true;
}

bool vma_remove(VmaNode*& root, VmaNode* node) {
  if (!node || vma_find_exact(root, node->startPage) != node) {
    return false;
  }
  VmaNode* parent = node->parent;
  bool leftShrank = parent && parent->left == node;
  if (!node->left || !node->right) {
    replace_child(root, node, node->left ? node->left : node->right);
  } else {
    VmaNode* successor = first_node(node->right);
    if (successor->parent == node) {
      parent = successor;
      leftShrank = false;
    } else {
      parent = successor->parent;
      leftShrank = true;
      replace_child(root, successor, successor->right);
      successor->right = node->right;
      successor->right->parent = successor;
    }
    replace_child(root, node, successor);
    successor->left = node->left;
    successor->left->parent = successor;
    successor->balance = node->balance;
  }
  node->left = node->right = node->parent = nullptr;
  node->balance = 0;
  node->linked = false;

  while (parent) {
    parent->balance += leftShrank ? 1 : -1;
    VmaNode* subtree = parent;
    if (parent->balance < -1 || parent->balance > 1) {
      subtree = rebalance(root, parent);
    }
    // A nonzero balance here means this subtree's height did not decrease.
    if (subtree->balance) {
      break;
    }
    parent = subtree->parent;
    leftShrank = parent && parent->left == subtree;
  }
  return true;
}

const VmaNode* vma_find(const VmaNode* root, u32 page) {
  while (root) {
    if (page < root->startPage) {
      root = root->left;
    } else if (page - root->startPage < root->pageCount) {
      return root;
    } else {
      root = root->right;
    }
  }
  return nullptr;
}

VmaNode* vma_find(VmaNode* root, u32 page) {
  return const_cast<VmaNode*>(vma_find(static_cast<const VmaNode*>(root), page));
}

VmaNode* vma_find_exact(VmaNode* root, u32 startPage) {
  VmaNode* node = vma_lower_bound(root, startPage);
  return node && node->startPage == startPage ? node : nullptr;
}

VmaNode* vma_lower_bound(VmaNode* root, u32 page) {
  VmaNode* next = nullptr;
  while (root) {
    if (root->startPage >= page) {
      next = root;
      root = root->left;
    } else {
      root = root->right;
    }
  }
  return next;
}

VmaNode* vma_first(VmaNode* root) {
  return first_node(root);
}

VmaNode* vma_next(VmaNode* node) {
  return node ? next_node(node) : nullptr;
}

const VmaNode* vma_next(const VmaNode* node) {
  return node ? next_node(node) : nullptr;
}

bool vma_find_gap(VmaNode* root, u32 beginPage, u32 endPage, u32 pageCount, u32& startPage) {
  if (beginPage >= endPage || endPage > PageLimit || !pageCount || pageCount > endPage - beginPage) {
    return false;
  }
  VmaNode* node = vma_find(root, beginPage);
  if (!node) {
    node = vma_lower_bound(root, beginPage);
  }
  u32 candidate = beginPage;
  for (; node && node->startPage < endPage; node = vma_next(node)) {
    if (node->startPage > candidate && pageCount <= node->startPage - candidate) {
      startPage = candidate;
      return true;
    }
    candidate = node->startPage + node->pageCount;
    if (candidate >= endPage) {
      return false;
    }
  }
  if (pageCount > endPage - candidate) {
    return false;
  }
  startPage = candidate;
  return true;
}

u32 vma_destroy(VmaNode*& root) {
  u32 count = 0;
  for (VmaNode* node = root; node;) {
    if (node->left) {
      node = node->left;
    } else if (node->right) {
      node = node->right;
    } else {
      VmaNode* parent = node->parent;
      replace_child(root, node, nullptr);
      kfree(node);
      node = parent;
      ++count;
    }
  }
  return count;
}

u32 vma_count(const VmaNode* root) {
  u32 count = 0;
  for (const VmaNode* node = first_node(root); node; node = next_node(node)) {
    ++count;
  }
  return count;
}

bool vma_check(const VmaNode* root) {
  return checked_height(root, nullptr, 0, PageLimit, CheckDepth) >= 0;
}

void vma_selftest() {
  check_intervals();
  check_mutations(false);
  check_mutations(true);
  pr_debug("VMA tree self-check passed\n");
}

}
