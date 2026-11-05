#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/terminal_edit_model.h"
static TerminalModel m;static TerminalEditView v;
int main(void){terminal_model_init(&m,8,3);TerminalEdit f={0};
 memcpy(f.text,"$ ab你cd",9);f.bytes=9;f.cursor=4;
 terminal_edit_apply(&m,&v,&f);assert(v.active&&v.caret_line==0&&v.caret_column==4);assert(m.line==1);
 memcpy(f.text,"$ a",3);f.bytes=f.cursor=3;terminal_edit_apply(&m,&v,&f);assert(m.line==0&&m.column==3&&m.used==1);
 f.action=1;terminal_edit_apply(&m,&v,&f);assert(!v.active&&m.line==1&&m.used==2);
 f.action=2;terminal_edit_apply(&m,&v,&f);assert(v.active&&m.line==0&&m.used==1);
 f.action=1;terminal_edit_apply(&m,&v,&f);
 for(int i=0;i<150;i++)terminal_model_codepoint(&m,'\n');
 f.action=0;f.bytes=f.cursor=30;memset(f.text,'x',30);terminal_edit_apply(&m,&v,&f);
 f.bytes=f.cursor=1;terminal_edit_apply(&m,&v,&f);assert(m.used<=128&&v.active);
 puts("LINE MODEL PASS");return 0;}
