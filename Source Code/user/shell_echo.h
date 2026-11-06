#ifndef SINO_SHELL_ECHO_H
#define SINO_SHELL_ECHO_H
#include "shell_commands.h"

/* Format completely before writing to a stream or publishing a file. */
static int shell_echo(const char* args, const u32* offsets, u32 count,
                      char* output, u32 capacity) {
    u32 first = 0, used = 0;
    int newline = 1, escapes = 0;
    while (first < count) {
        const char* arg = args + offsets[first];
        if (shell_equal(arg, "-n"))
            newline = 0;
        else if (shell_equal(arg, "-e"))
            escapes = 1;
        else
            break;
        first++;
    }
    for (u32 i = first; i < count; i++) {
        if (i > first) {
            if (used == capacity) return -1;
            output[used++] = ' ';
        }
        for (const char* p = args + offsets[i]; *p; p++) {
            char c = *p;
            if (escapes && c == '\\') {
                c = *++p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c != '\\') return -1;
            }
            if (used == capacity) return -1;
            output[used++] = c;
        }
    }
    if (newline) {
        if (used == capacity) return -1;
        output[used++] = '\n';
    }
    return (int)used;
}
#endif
