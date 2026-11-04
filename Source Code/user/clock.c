/* Clock presentation and refresh policy run entirely in Ring 3. */
#include "sdk.h"
static UiCanvas canvas;
static int window,language;
static const int sine[60]={0,10,21,31,41,50,59,67,74,81,87,91,95,98,99,100,99,98,95,91,87,81,74,67,59,50,41,31,21,10,0,-10,-21,-31,-41,-50,-59,-67,-74,-81,-87,-91,-95,-98,-99,-100,-99,-98,-95,-91,-87,-81,-74,-67,-59,-50,-41,-31,-21,-10};
static void text_time(const TimeOfDay *t,char *out){out[0]='0'+t->hour/10;out[1]='0'+t->hour%10;out[2]=':';out[3]='0'+t->minute/10;out[4]='0'+t->minute%10;out[5]=':';out[6]='0'+t->second/10;out[7]='0'+t->second%10;out[8]=0;}
static int line(int pos,int length,u32 color){return ui_canvas_line(&canvas,165,171,165+sine[pos%60]*length/100,171-sine[(pos+15)%60]*length/100,color);}
static int draw(const TimeOfDay *time,int available){
 ui_canvas_begin(&canvas);
 if(ui_canvas_fill(&canvas,2,30,326,338,0xd6d7db)<0||ui_canvas_ellipse(&canvas,56,62,219,219,0x858d99)<0||ui_canvas_ellipse(&canvas,59,65,213,213,0xf7f7f3)<0)return 0;
 for(int i=0;i<60;i++){int length=i%5?100:94;if(ui_canvas_line(&canvas,165+sine[i]*length/100,171-sine[(i+15)%60]*length/100,165+sine[i]*104/100,171-sine[(i+15)%60]*104/100,i%5?0xb8bec6:0x24252a)<0)return 0;}
 if(time&&(line((time->hour%12)*5+time->minute/12,56,0x24252a)<0||line(time->minute,84,0x24252a)<0||line(time->second,94,0xb34445)<0))return 0;
 char text[9];if(time)text_time(time,text);else copy(text,"--:--:--");
 if(ui_canvas_ellipse(&canvas,161,167,9,9,0x24252a)<0||ui_canvas_text(&canvas,110,297,1,text,0x24252a)<0||ui_canvas_text(&canvas,105,337,0,available?(language?"Hardware clock":"本地硬件时钟"):(language?"Time unavailable":"时间暂不可用"),0x717984)<0)return 0;
 return Scene(window,(const UiScene *)&canvas)==0;
}
static int same(const TimeOfDay *a,const TimeOfDay *b){return a->hour==b->hour&&a->minute==b->minute&&a->second==b->second;}
static int selftest(void){
 TimeOfDay t;int r=Time(&t);if(r||t.hour>23||t.minute>59||t.second>59||Write("/home/user/clock-denied.txt","x",1)!=S_DENIED||api(SYS_SOCKET,1,0,0)!=S_DENIED||api(SYS_KILL,0x7fffffffu,0,0)!=S_DENIED){Log("CLOCK SELFTEST FAIL\n");return 1;}
 Log("CLOCK SELFTEST PASS: wall time and zero capabilities\n");return 0;
}
int main(void){
 if(streq(Arguments(),"/selftest"))return selftest();
 if(streq(Arguments(),"/spawn-test"))return SpawnArgs("/boot/clock.snx","/selftest")>0?0:1;
 language=UiLanguage();if(language<0||language>1)return 1;
 window=Window(language?"Clock":"时钟",330,370);if(window<0){Log("CLOCK WINDOW FAILED\n");return 1;}
 if(api(SYS_WINDOW_POLICY,(u32)window,WINDOW_NO_IME,0)<0)return 1;
 TimeOfDay last={0,0,0};int known=0,available=0,first=1;u32 checked=(u32)api(SYS_TICKS,0,0,0)-10;
 for(;;){
  UiEvent event;if(Event(&event)==0&&(event.type==MSG_CLOSE||(event.type==MSG_KEY&&event.key==27)))return 0;
  u32 now=(u32)api(SYS_TICKS,0,0,0);
  if(now-checked>=10){
   checked=now;TimeOfDay next;int r=Time(&next),next_language=UiLanguage();if(r&&r!=S_AGAIN){Log("CLOCK TIME FAILED\n");return 1;}if(next_language<0||next_language>1)return 1;
   int changed=first||(r==0)!=available||next_language!=language;
   if(r==0){if(!known||!same(&next,&last))changed=1;last=next;known=1;}
   if(next_language!=language){language=next_language;if(WindowTitle(window,language?"Clock":"时钟")<0)return 1;}
   available=r==0;if(changed&&!draw(known?&last:0,available)){Log("CLOCK SCENE FAILED\n");return 1;}
   if(first&&streq(Arguments(),"/fault-test")){Yield(20);__asm__ volatile("ud2");}
   first=0;
  }Yield(1);
 }
}
