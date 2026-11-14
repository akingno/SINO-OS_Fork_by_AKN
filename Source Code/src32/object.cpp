#include "klog.h"
#include "object.h"
#include "identifiers.h"
#include "task/wait.h"
#include "net.h"
#include "task/task.h"
#include "filesys.h"
#include "pipe.h"
#include "terminal.h"
#include "lib/kstring.h"
#include "panic.h"
Object objects[OBJECTS];
extern "C" { ids::Stats identifier_stats; }
static ids::Cookies<HANDLES, 0x7fffffu> cookies;
int object_new(ObjType type, int value) {
  for (int i = 0; i < OBJECTS; i++) {
    if (!objects[i].refs && objects[i].generation != 0xffffffffu) {
      // 每槽独立的完整 32 位代数；最后一代销毁后退役，不重新变成零。
      ids::Counter<0xffffffffu> counter{objects[i].generation};
      objects[i] = Object{counter.issue(), 1, type, value};
      identifier_stats.objects++;
      task::objectCreated((u32)i + 1);
      return i;
    }
  }
  identifier_stats.objectLimits++;
  return S_LIMIT;
}
int handle_open(Handle* table, int index, u32 rights) {
  if (index < 0 || index >= OBJECTS || !objects[index].refs) { return S_BADHANDLE; }
  u32 freeMask = 0;
  for (u32 i = 0; i < HANDLES; i++) {
    if (!table[i].generation) { freeMask |= 1u << i; }
  }
  int slot = cookies.choose(freeMask);
  if (slot < 0) { identifier_stats.handleLimits++; return S_LIMIT; }
  u32 cookie = cookies.issue((u32)slot);
  table[slot] = Handle{(u32)index, objects[index].generation, rights, cookie};
  identifier_stats.handles++;
  return (int)((cookie << 8) | (u32)(slot + 1));
}
Object* handle_get(Handle* table, u32 h, ObjType type, u32 rights) {
  u32 slot = (h & 255) - 1;
  if (slot >= HANDLES)
    return 0;
  Handle* r = &table[slot];
  if (!r->generation || r->cookie != (h >> 8) || r->object >= OBJECTS ||
      (r->rights & rights) != rights)
    return 0;
  Object* o = &objects[r->object];
  return o->refs && o->generation == r->generation && o->type == type ? o : 0;
}
/* Drop either a handle reference or a kernel execution reference. */
void object_unref(int index) {
  if (index < 0 || index >= OBJECTS || !objects[index].refs) return;
  Object* o = &objects[index];
  if (!--o->refs) {
    task::objectClosed((u32)index + 1);
    if (o->type == ObjType::SOCKET) net_close(o->value);
    else if (o->type == ObjType::PROCESS) process_completion_release(o->value);
    else if (o->type == ObjType::STREAM) process_stream_release(o->value);
    else if (o->type == ObjType::DIRECTORY || o->type == ObjType::FILE || o->type == ObjType::STAGING)
      filesys_release(o->value);
    else if (o->type == ObjType::PIPE_READ || o->type == ObjType::PIPE_WRITE)
      pipe_release(o);
    else if(o->type==ObjType::TERM_CONTROL||o->type==ObjType::TERM_CLIENT)
      terminal_release(o->value,o->type==ObjType::TERM_CLIENT);
  }
}
int handle_close(Handle* table, u32 h) {
  u32 slot = (h & 255) - 1;
  if (slot >= HANDLES)
    return S_BADHANDLE;
  Handle* r = &table[slot];
  if (!r->generation || r->cookie != (h >> 8))
    return S_BADHANDLE;
  Object* o = &objects[r->object];
  if (o->generation != r->generation || !o->refs)
    return S_BADHANDLE;
  memset(r, 0, sizeof(*r));
  object_unref((int)(o - objects));
  return 0;
}
int handle_duplicate(Handle* t, u32 h, u32 rights) {
  u32 slot = (h & 255) - 1;
  if (slot >= HANDLES || !t[slot].generation || t[slot].cookie != (h >> 8) ||
      !rights || (rights & ~t[slot].rights))
    return S_BADHANDLE;
  Object* o = &objects[t[slot].object];
  if (!o->refs || o->generation != t[slot].generation)
    return S_BADHANDLE;
  int result = handle_open(t, (int)t[slot].object, rights);
  if (result >= 0)
    o->refs++;
  return result;
}
void handles_close(Handle* t) {
  for (u32 i = 0; i < HANDLES; i++)
    if (t[i].generation)
      handle_close(t, (t[i].cookie << 8) | (i + 1));
}
#ifdef SINO_SHELL_BRIDGE
static void identifierBoundaryTests() {
  // 启动、无用户对象时暂存元数据；从不回退已经发布给用户的编号。
  Object saved[OBJECTS];
  for (const auto& object : objects) {
    if (object.refs) { panic("Identifier test needs empty objects", 0, 0); }
  }
  memcpy(saved, objects, sizeof(saved));
  const auto savedCookies = cookies;
  const auto savedStats = identifier_stats;
  Handle table[HANDLES]{};
  for (auto& object : objects) { object.generation = 0xffffffffu; }
  objects[0].generation--;
  for (auto& counter : cookies.counters) { counter.value = 0x7fffffu; }
  cookies.counters[HANDLES - 1].value--;
  int object = object_new(ObjType::EVENT, 0);
  int handle = handle_open(table, object, 3);
  if (object != 0 || objects[0].generation != 0xffffffffu || handle <= 0 ||
      (u32)handle != ((0x7fffffu << 8) | HANDLES) ||
      !handle_get(table, (u32)handle, ObjType::EVENT, 1) ||
      handle_duplicate(table, (u32)handle, 1) != S_LIMIT || objects[0].refs != 1 ||
      handle_close(table, (u32)handle) != S_OK ||
      handle_get(table, (u32)handle, ObjType::EVENT, 1) ||
      object_new(ObjType::EVENT, 0) != S_LIMIT) {
    panic("Identifier exhaustion", 0, 0);
  }
  objects[1].generation = 0;
  object = object_new(ObjType::EVENT, 1);
  if (object != 1 || handle_open(table, object, 1) != S_LIMIT || objects[1].refs != 1) {
    panic("Retired identifier slot", 0, 0);
  }
  object_unref(object);
  memcpy(objects, saved, sizeof(saved));
  cookies = savedCookies;
  identifier_stats = savedStats;
  // objectCreated 在下次创建时重新绑定空桶；测试从未登记等待者。
  pr_debug("IDENTIFIERS object/cookie exhaustion PASS\n");
}
#endif
void object_tests(void) {
  Handle t[HANDLES];
  memset(t, 0, sizeof(t));
  int o = object_new(ObjType::EVENT, 0), h = handle_open(t, o, 3);
  if (h < 0 || !handle_get(t, (u32)h, ObjType::EVENT, 1) ||
      handle_get(t, (u32)h, ObjType::WINDOW, 1) ||
      handle_get(t, (u32)h, ObjType::EVENT, 4) || handle_close(t, (u32)h) ||
      handle_get(t, (u32)h, ObjType::EVENT, 1) ||
      handle_close(t, (u32)h) != S_BADHANDLE)
    panic("Handle validation", 0, 0);
  int b = object_new(ObjType::EVENT, 1), k = handle_open(t, b, 1);
  if (k == h || handle_get(t, (u32)h, ObjType::EVENT, 1))
    panic("Stale handle", 0, 0);
  int duplicate = handle_duplicate(t, (u32)k, 1);
  if (duplicate < 0 || objects[b].refs != 2 ||
      handle_duplicate(t, (u32)k, 2) != S_BADHANDLE)
    panic("Handle duplication rights", 0, 0);
  handle_close(t, (u32)k);
  int newer = handle_duplicate(t, (u32)duplicate, 1);
  if (newer == k || handle_get(t, (u32)k, ObjType::EVENT, 1))
    panic("Duplicated stale slot", 0, 0);
  handles_close(t);
  if (objects[b].refs)
    panic("Object reference release", 0, 0);
  pr_debug("OBJECT handles rights/stale/release PASS\n");
#ifdef SINO_SHELL_BRIDGE
  identifierBoundaryTests();
#endif
}
