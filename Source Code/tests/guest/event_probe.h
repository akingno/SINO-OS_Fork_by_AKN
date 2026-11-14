#ifndef SINO_EVENT_PROBE_H
#define SINO_EVENT_PROBE_H
namespace eventprobe {
bool matches(const char* mode);
int run(const char* mode);
}
#endif
