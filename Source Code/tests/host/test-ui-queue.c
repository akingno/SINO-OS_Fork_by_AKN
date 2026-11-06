#include <assert.h>
#include <stdio.h>
#include "../../src32/ui_queue.h"
int main(void){UiQueue q={0};UiEvent e;
 for(int i=0;i<15;i++)ui_queue_push(&q,(UiEvent){MSG_TEXT,1,0,0,i},1);
 ui_queue_push(&q,(UiEvent){MSG_TEXT,1,0,0,99},1);
 assert(!ui_queue_pop(&q,&e)&&e.type==MSG_INPUT_REJECTED&&e.key==1);
 assert(ui_queue_pop(&q,&e)==S_AGAIN);
 for(int i=0;i<20;i++)ui_queue_push(&q,(UiEvent){MSG_TEXT,1,0,0,i},1);
 ui_queue_push(&q,(UiEvent){MSG_KEY,1,0,0,3},1);ui_queue_push(&q,(UiEvent){MSG_CLOSE,1,0,0,0},1);
 assert(!ui_queue_pop(&q,&e)&&e.type==MSG_CLOSE);
 assert(!ui_queue_pop(&q,&e)&&e.type==MSG_KEY&&e.key==3);
 assert(!ui_queue_pop(&q,&e)&&e.type==MSG_INPUT_REJECTED);
 for(int i=0;i<15;i++)ui_queue_push(&q,(UiEvent){MSG_KEY,1,0,0,i},0);
 ui_queue_push(&q,(UiEvent){MSG_KEY,1,0,0,99},0);
 for(int i=0;i<15;i++)assert(!ui_queue_pop(&q,&e)&&e.key==i);
 assert(ui_queue_pop(&q,&e)==S_AGAIN);
 puts("UI QUEUE PASS");return 0;}
