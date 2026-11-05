#ifndef SINO_INTERACTIVE_ABI_H
#define SINO_INTERACTIVE_ABI_H
#include "api.h"
#define TERM_INTERRUPT 7u
enum { IX_SHELL = 3, IX_COMMAND = 4, IX_READTOOLS = 5 };
enum { IX_START = 12, IX_REQUEST, IX_SUBMIT, IX_RESULT, IX_FINISH };
typedef struct {
  u32 size, version, program, id, argc, bytes, rights;
  u32 offsets[16];
  char argv[1024];
} InteractiveSpec;
typedef struct {
  u32 size, version, id;
  i32 status;
  ProcessCompletion completion;
} InteractiveResult;
_Static_assert(sizeof(InteractiveSpec) == 1116, "interactive request ABI");
_Static_assert(sizeof(InteractiveResult) == 48, "interactive result ABI");
#endif
