/* Independent ABI-1 calculator. All arithmetic and presentation live in CPL3. */
#include "sdk.h"
#define LIMIT 9999999
typedef struct {i32 value,acc;int op,entry,decimal,digits,error;} Calculator;
static Calculator state;
static UiScene scene;
static int window,language;
static const char *keys[]={"C","+/-","/","*","7","8","9","-","4","5","6","+","1","2","3","=","0",".","<","="};
static void format(const Calculator *s,char *out){i32 v=s->value;if(v<0){*out++='-';v=-v;}number((u32)v/100,out);out+=strlen(out);*out++='.';*out++='0'+v/10%10;*out++='0'+v%10;*out=0;}
static void apply(Calculator *s){
 i32 a=s->acc,b=s->value,result=b;
 if(s->op=='+')result=a+b;
 else if(s->op=='-')result=a-b;
 else if(s->op=='*'){
  int negative=(a<0)^(b<0);u32 x=(u32)(a<0?-a:a),y=(u32)(b<0?-b:b),whole=x/100;
  if(whole&&y>LIMIT/whole){s->error=1;return;}
  u32 product=whole*y+(x%100)*y/100; /* fractional intermediate <= 989999901 */
  if(product>LIMIT){s->error=1;return;}result=negative?-(i32)product:(i32)product;
 }else if(s->op=='/'){
  if(!b){s->error=1;return;}result=(a/b)*100+(a%b)*100/b;
 }
 if(result>LIMIT||result< -LIMIT){s->error=1;return;}s->value=result;
}
static void key(Calculator *s,int k){
 if(k=='C'||k=='c'||k==27){memset(s,0,sizeof(*s));s->entry=1;return;}
 if(s->error)return;
 if(k>='0'&&k<='9'){
  if(s->entry){s->value=s->decimal=s->digits=0;s->entry=0;}
  int d=k-'0',sign=s->value<0?-1:1;
  if(s->decimal){if(s->digits<2){s->value+=sign*d*(s->digits?1:10);s->digits++;}}
  else if((s->value<0?-s->value:s->value)<=999999)s->value=s->value*10+sign*d*100;
  return;
 }
 if(k=='.'){if(s->entry){s->value=0;s->entry=0;}s->decimal=1;return;}
 if(k==8){if(s->decimal&&s->digits){s->value-=s->value%(s->digits==2?10:100);s->digits--;}else if(s->decimal)s->decimal=0;else s->value=(s->value/100/10)*100;return;}
 if(k=='n'){s->value=-s->value;return;}
 if(k=='+'||k=='-'||k=='*'||k=='/'||k=='='||k==13){if(s->op&&!s->entry)apply(s);s->acc=s->value;s->op=(k=='='||k==13)?0:k;s->entry=1;s->decimal=s->digits=0;}
}
static void check(int ok,const char *reason){if(!ok){Log("CALCULATOR SELFTEST FAIL: ");Log(reason);Log("\n");Exit(1);}}
static int selftest(void){
 static const struct {const char *input,*output;int error;} cases[]={
  {"12.5+7.25=","19.75",0},{"99999/3=","33333.00",0},{"99999*1=","99999.00",0},
  {"6*7=","42.00",0},{"1/0=","1.00",1},{"1/3=","0.33",0},{"99999.99+0.01=","0.01",1},
  {"0.5n","-0.50",0},{"9-12=","-3.00",0},{"2+3*4=","20.00",0},
  {"99999.99*99999.99=","99999.99",1},{"99999.99/0.01=","0.01",1},
  {"0.99*99999.99=","98999.99",0},{"99999.99n/0.01=","0.01",1},{"1n/3=","-0.33",0}
 };
 for(u32 i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  Calculator s;key(&s,'c');for(u32 j=0;cases[i].input[j];j++)key(&s,cases[i].input[j]);char text[24];format(&s,text);
  check(s.error==cases[i].error&&(s.error||streq(text,cases[i].output)),cases[i].input);
  Log("CALC CASE ");Log(cases[i].input);Log(" -> ");Log(s.error?"Error":text);Log("\n");
 }
 check(Write("/home/user/calc-denied.txt","x",1)==S_DENIED,"home default deny");
 check(api(SYS_SOCKET,1,0,0)==S_DENIED,"network default deny");
 check(api(SYS_KILL,0x7fffffffu,0,0)==S_DENIED,"manage default deny");
 check(UiLanguage()>=0&&UiLanguage()<=1,"language");
 Log("CALCULATOR SELFTEST PASS: arithmetic and zero-capability process\n");return 0;
}
static int draw(void){
 char value[24];format(&state,value);ui_begin(&scene);
 ui_fill(&scene,2,30,302,374,0xd6d7db);ui_panel(&scene,16,48,274,59,0xf6f8ef,1);
 ui_text(&scene,26,65,state.error?(language?"Error":"错误"):value);scene.commands[scene.count-1].h=1;
 for(int i=0;i<20;i++){int x=16+(i%4)*70,y=124+(i/4)*49;ui_panel(&scene,x,y,62,40,0xd6d7db,0);ui_text(&scene,x+20,y+9,keys[i]);}
 ui_text(&scene,18,378,language?"Two decimal places":"支持两位小数计算");
 return scene.count<=UI_COMMANDS&&Scene(window,&scene)==0;
}
int main(void){
 if(streq(Arguments(),"/selftest"))return selftest();
 if(streq(Arguments(),"/spawn-test")){int pid=SpawnArgs("/boot/calc.snx","/selftest");check(pid>0,"child spawn");return 0;}
 language=UiLanguage();if(language<0||language>1)return 1;
 window=Window(language?"Calculator":"计算器",306,406);
 if(window<0){Log("CALCULATOR WINDOW FAILED\n");return 1;}
 if(api(SYS_WINDOW_POLICY,(u32)window,WINDOW_NO_IME,0)<0)return 1;
 key(&state,'c');if(!draw()){Log("CALCULATOR SCENE FAILED\n");return 1;}
 if(streq(Arguments(),"/title-test")){char handle[12];number((u32)window,handle);Log("CALC TITLE TEST HANDLE ");Log(handle);Log("\n");}
 u32 checked=(u32)api(SYS_TICKS,0,0,0);
 for(;;){
  int changed=0;UiEvent e;
  if(Event(&e)==0){
   Calculator before=state;
   if(e.type==MSG_KEY)key(&state,e.key);
   else if(e.type==MSG_CLOSE)return 0;
   else if(e.type==MSG_CLICK)for(int i=0;i<20;i++)if(ui_hit(&e,16+(i%4)*70,124+(i/4)*49,62,40)){key(&state,i==1?'n':i==18?8:keys[i][0]);break;}
   changed=before.value!=state.value||before.error!=state.error;
  }
  u32 now=(u32)api(SYS_TICKS,0,0,0);
  if(now-checked>=10){checked=now;int next=UiLanguage();if(next<0||next>1)return 1;if(next!=language){language=next;if(WindowTitle(window,language?"Calculator":"计算器")<0)return 1;changed=1;}}
  if(changed&&!draw()){Log("CALCULATOR SCENE FAILED\n");return 1;}Yield(1);
 }
}
