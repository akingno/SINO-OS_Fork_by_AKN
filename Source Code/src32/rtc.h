#ifndef SINO_RTC_H
#define SINO_RTC_H
#include "api.h"
typedef struct {
  u32 year, month, day, hour, minute, second;
} RtcDateTime;
void rtc_init(void);
void rtc_poll(void);
int rtc_read(RtcDateTime *);
int rtc_time(TimeOfDay *);
#endif
