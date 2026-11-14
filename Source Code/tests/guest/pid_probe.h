#ifndef SINO_PID_PROBE_H
#define SINO_PID_PROBE_H
namespace pidprobe {
bool matches(const char* mode);
int run(const char* mode);
}
#endif
