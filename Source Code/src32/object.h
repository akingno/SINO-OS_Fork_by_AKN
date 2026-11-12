#ifndef SINO_OBJECT_H
#define SINO_OBJECT_H
#include "api.h"
#ifdef __cplusplus
extern "C" {
#endif
#define OBJECTS 64
#define HANDLES 24
enum class ObjType : u32 { EVENT = 1, WINDOW, SOCKET, PROCESS, STREAM,
       DIRECTORY, FILE, STAGING, PIPE_READ, PIPE_WRITE,
       TERM_CONTROL, TERM_CLIENT };
typedef struct {
  u32 object, generation, rights, cookie;
} Handle;
typedef struct {
  u32 generation, refs;
  ObjType type;
  int value;
} Object;
extern Object objects[OBJECTS];
int object_new(ObjType, int);
void object_unref(int);
int handle_open(Handle *, int, u32);
Object *handle_get(Handle *, u32, ObjType, u32);
int handle_duplicate(Handle *, u32, u32);
int handle_close(Handle *, u32);
void handles_close(Handle *);
void object_tests(void);
#ifdef __cplusplus
}
#endif
#endif
