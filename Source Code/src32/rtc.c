/* Bounded RTC sampling. The cache is shared by the desktop, Shell and SYS_TIME.
 */
#include "rtc.h"
#ifdef SINO_RTC_HOST_TEST
extern volatile u32 ticks;
extern u8 rtc_test_read(u8);
#define read_register rtc_test_read
#else
#include "kernel.h"
/* This driver keeps NMI enabled. No other subsystem selects CMOS registers. */
static u8 read_register(u8 index) {
  outb(0x70, index & 127);
  return inb(0x71);
}
#endif
typedef struct {
  u8 second, minute, hour, day, month, year, century, mode;
} RawTime;
static RtcDateTime cached;
static u32 sampled, last_attempt;
static int valid, attempted;
static int decimal(u8 raw, int binary) {
  if (binary)
    return raw;
  if ((raw & 15) > 9 || (raw >> 4) > 9)
    return -1;
  return (raw >> 4) * 10 + (raw & 15);
}
static int decode(const RawTime *raw, RtcDateTime *out) {
  int binary = raw->mode & 4, second = decimal(raw->second, binary),
      minute = decimal(raw->minute, binary);
  int hour = decimal((raw->mode & 2) ? raw->hour : (raw->hour & 127), binary);
  int day = decimal(raw->day, binary), month = decimal(raw->month, binary),
      year = decimal(raw->year, binary);
  int century = (raw->century == 0 || raw->century == 255)
                    ? 20
                    : decimal(raw->century, binary);
  if (second < 0 || second > 59 || minute < 0 || minute > 59 || year < 0 ||
      year > 99 || century < 19 || century > 99 || month < 1 || month > 12)
    return 0;
  if (raw->mode & 2) {
    if (hour < 0 || hour > 23)
      return 0;
  } else {
    if (hour < 1 || hour > 12)
      return 0;
    hour = hour % 12 + ((raw->hour & 128) ? 12 : 0);
  }
  u32 full_year = (u32)(century * 100 + year);
  static const u8 days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max_day = days[month - 1];
  if (month == 2 && full_year % 4 == 0 &&
      (full_year % 100 != 0 || full_year % 400 == 0))
    max_day++;
  if (day < 1 || day > max_day)
    return 0;
  *out = (RtcDateTime){full_year, (u32)month,  (u32)day,
                       (u32)hour, (u32)minute, (u32)second};
  return 1;
}
static int snapshot(RawTime *raw) {
  int ready = 0;
  for (int i = 0; i < 1024; i++)
    if (!(read_register(0x0a) & 128)) {
      ready = 1;
      break;
    }
  if (!ready)
    return 0;
  raw->mode = read_register(0x0b);
  raw->second = read_register(0);
  raw->minute = read_register(2);
  raw->hour = read_register(4);
  raw->day = read_register(7);
  raw->month = read_register(8);
  raw->year = read_register(9);
  raw->century = read_register(0x32);
  return !(read_register(0x0a) & 128) && read_register(0x0b) == raw->mode;
}
static int sample(RtcDateTime *out) {
  for (int tries = 0; tries < 4; tries++) {
    RawTime a, b;
    if (!snapshot(&a) || !snapshot(&b))
      continue;
    const u8 *x = (const u8 *)&a, *y = (const u8 *)&b;
    int equal = 1;
    for (u32 i = 0; i < sizeof(a); i++)
      if (x[i] != y[i])
        equal = 0;
    if (equal && decode(&a, out))
      return 1;
  }
  return 0;
}
void rtc_poll(void) {
  u32 now = ticks;
  if (attempted && now - last_attempt < 100)
    return;
  attempted = 1;
  last_attempt = now;
  RtcDateTime next;
  if (sample(&next)) {
    cached = next;
    sampled = now;
    valid = 1;
  }
#ifndef SINO_RTC_HOST_TEST
  outb(0x70, 0); /* Leave a known selector and NMI enabled, also on failure. */
#endif
}
void rtc_init(void) {
  valid = attempted = 0;
  rtc_poll();
}
int rtc_read(RtcDateTime *out) {
  if (!valid || (u32)(ticks - sampled) > 200)
    return S_AGAIN;
  *out = cached;
  return 0;
}
int rtc_time(TimeOfDay *out) {
  RtcDateTime value;
  int r = rtc_read(&value);
  if (r)
    return r;
  *out = (TimeOfDay){value.hour, value.minute, value.second};
  return 0;
}
