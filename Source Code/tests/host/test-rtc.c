/* Compile the actual driver with a register backend; no duplicate decoder. */
#define SINO_RTC_HOST_TEST 1
#include "../../src32/rtc.c"
#include <stdio.h>
#include <stdlib.h>
volatile u32 ticks;
static u8 registers[128];static int busy,unstable,mode_change,reads;
u8 rtc_test_read(u8 index){reads++;if(index==10&&busy)return 128;if(index==0&&unstable)return (u8)(reads&1);if(index==11&&mode_change)return (u8)((reads&1)?2:6);return registers[index];}
static int checks;
static void check(int ok,const char *name){if(!ok){fprintf(stderr,"FAIL RTC: %s\n",name);exit(1);}checks++;}
static RawTime raw(u8 h,u8 m,u8 s,u8 mode){RawTime value={s,m,h,3,11,26,20,mode};if(!(mode&4)){value.month=0x11;value.year=0x26;value.century=0x20;}return value;}
static void load(void){for(int i=0;i<128;i++)registers[i]=0;registers[11]=6;registers[4]=12;registers[2]=34;registers[0]=56;registers[7]=3;registers[8]=11;registers[9]=26;registers[0x32]=20;busy=unstable=mode_change=reads=0;}
int main(void){
 RtcDateTime t;RawTime value=raw(23,59,59,6);check(decode(&value,&t)&&t.hour==23&&t.second==59,"binary 24h");
 value=raw(0x23,0x59,0x59,2);value.day=3;value.month=0x11;value.year=0x26;value.century=0x20;check(decode(&value,&t)&&t.year==2026&&t.month==11&&t.hour==23,"BCD 24h");
 value=raw(12,0,0,4);check(decode(&value,&t)&&t.hour==0,"12 AM");value.hour=12|128;check(decode(&value,&t)&&t.hour==12,"12 PM");value.hour=1|128;check(decode(&value,&t)&&t.hour==13,"1 PM");
 value=raw(0x92,0,0,0);value.month=0x11;value.year=0x26;value.century=0x20;check(decode(&value,&t)&&t.hour==12,"BCD PM");
 value=raw(0,0,0,6);check(decode(&value,&t)&&!t.hour&&!t.second,"midnight");value=raw(11,59,59,6);check(decode(&value,&t)&&t.hour==11,"before noon");value=raw(12,0,0,6);check(decode(&value,&t)&&t.hour==12,"noon");
 value.hour=24;check(!decode(&value,&t),"invalid hour");value=raw(0,0,60,6);check(!decode(&value,&t),"invalid second");value=raw(0x1a,0,0,2);check(!decode(&value,&t),"invalid BCD");value=raw(0,0,0,4);check(!decode(&value,&t),"zero 12h hour");
 value=raw(0,0,0,6);value.month=2;value.day=29;value.year=0;check(decode(&value,&t),"2000 leap day");value.century=19;check(!decode(&value,&t),"1900 not leap");value.day=30;check(!decode(&value,&t),"invalid date");value.month=13;check(!decode(&value,&t),"invalid month");
 value=raw(0,0,0,6);value.century=255;check(decode(&value,&t)&&t.year==2026,"missing century fallback");
 load();ticks=0;rtc_init();check(rtc_read(&t)==0&&t.hour==12&&t.minute==34&&t.second==56,"stable shared cache");
 reads=0;for(int i=0;i<100;i++)check(rtc_read(&t)==0,"read cache");check(reads==0,"cache reads do not query hardware");
 busy=1;ticks=100;rtc_poll();check(reads<=4096,"UIP retry bound");ticks=200;check(rtc_read(&t)==0,"cache TTL boundary");ticks=201;t=(RtcDateTime){1,1,1,1,1,1};check(rtc_read(&t)==S_AGAIN&&t.hour==1,"stale output untouched");
 ticks=300;TimeOfDay clock={77,77,77};check(rtc_time(&clock)==S_AGAIN&&clock.second==77,"time failure untouched");busy=0;registers[0]=57;rtc_poll();check(rtc_time(&clock)==0&&clock.second==57,"retry recovery");
 load();unstable=1;ticks=0;rtc_init();check(!valid,"unstable seconds rejected");
 load();mode_change=1;rtc_init();check(!valid,"mode changes rejected");
 load();ticks=0xfffffff0u;rtc_init();ticks=84;rtc_poll();check(rtc_read(&t)==0,"poll across ticks wrap");ticks=285;check(rtc_read(&t)==S_AGAIN,"TTL across ticks wrap");
 load();registers[0]=60;rtc_init();check(!valid,"invalid hardware sample never cached");
 printf("RTC DRIVER PASS: %d assertions, actual sampler/decoder/cache\n",checks);return 0;
}
