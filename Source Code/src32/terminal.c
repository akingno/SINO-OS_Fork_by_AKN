/* Independent headless text sessions. Frontends hold control, programs hold
   client handles. No ambient keyboard, screen or command-evaluator authority. */
#include "terminal.h"
#include "process.h"
typedef struct {
  u32 refs, width, height, mode, foreground, closed, eof;
  u32 input_size, output_size, line_size, events;
  u8 input[1024], output[1024], line[256];
  TerminalEvent event[8];
} Terminal;
typedef struct { Terminal* terminal; u32 pid; } Client;
_Static_assert(sizeof(Terminal)<=4096, "terminal metadata fits page");
static u32 count;
static void reset(Terminal* t) {
  t->mode=TERM_LINE|TERM_ECHO; t->foreground=0;
  t->input_size=t->line_size=t->events=t->eof=0;
}
static void unref(Terminal* t) { if (!--t->refs) {page_free(t);count--;} }
void terminal_release(int value,int client) {
  if (client) {
    Client* c=(Client*)value; Terminal* t=c->terminal;
    if(t->foreground==c->pid) reset(t);
    page_free(c);unref(t);
  } else { Terminal* t=(Terminal*)value;t->closed=1;unref(t); }
}
static int dimensions(u32 w,u32 h){return w&&w<=240&&h&&h<=100;}
static int create(Handle* owner,u32 w,u32 h) {
  if(!dimensions(w,h))return S_INVALID;
  if(count>=2)return S_LIMIT;
  Terminal* t=page_alloc();if(!t)return S_NOMEM;
  t->refs=1;t->width=w;t->height=h;reset(t);count++;
  int o=object_new(OBJ_TERM_CONTROL,(int)t);
  if(o<0){unref(t);return o;}
  int handle=handle_open(owner,o,1);if(handle<0)object_unref(o);
  return handle;
}
static Client* client(Handle* owner,u32 h,u32 pid) {
  Object* o=handle_get(owner,h,OBJ_TERM_CLIENT,1);
  if(!o)return 0;
  Client* c=(Client*)o->value;return c->pid==pid?c:0;
}
static int attach(Terminal* t,Handle* target,u32 pid) {
  if(t->closed)return S_CLOSED;
  int handle=0;
  for(u32 i=0;i<HANDLES;i++)if(target[i].generation){
    Object* o=&objects[target[i].object];
    if(o->type!=OBJ_TERM_CLIENT)continue;
    Client* c=(Client*)o->value;
    if(c->terminal!=t)return S_BUSY;
    handle=(int)((target[i].cookie<<8)|(i+1));break;
  }
  if(!handle){
    Client* c=page_alloc();if(!c)return S_NOMEM;
    c->terminal=t;c->pid=pid;
    int o=object_new(OBJ_TERM_CLIENT,(int)c);
    if(o<0){page_free(c);return o;}
    t->refs++;
    handle=handle_open(target,o,1);
    if(handle<0){object_unref(o);return handle;}
  }
  reset(t);t->foreground=pid;return handle;
}
static int pop(u8* from,u32* size,void* out,u32 bytes){
  if(bytes>*size)bytes=*size;
  memcpy(out,from,bytes);*size-=bytes;
  for(u32 i=0;i<*size;i++)from[i]=from[i+bytes];
  return (int)bytes;
}
static int event(Terminal* t,u32 type,u32 value,u32 w,u32 h){
  if(t->events==8)return S_AGAIN;
  t->event[t->events++]=(TerminalEvent){type,value,w,h};return 0;
}
static int feed(Terminal* t,const TerminalRequest* r){
  if(r->value<TERM_TEXT||r->value>TERM_RESIZE)return S_INVALID;
  if(t->closed)return S_CLOSED;
  if(r->value==TERM_CANCEL)return process_terminal_cancel(t->foreground);
  if(r->value==TERM_HANGUP){t->closed=1;event(t,TERM_HANGUP,0,0,0);return 0;}
  if(r->value==TERM_RESIZE){
    u32 w=r->extra>>16,h=r->extra&65535;
    if(!dimensions(w,h))return S_INVALID;
    int status=event(t,TERM_RESIZE,0,w,h);
    if(!status){t->width=w;t->height=h;}return status;
  }
  if(!t->foreground)return S_AGAIN;
  if(r->value==TERM_KEY){
    if(t->mode&TERM_LINE)return S_INVALID;
    return event(t,TERM_KEY,r->extra,0,0);
  }
  if(r->value==TERM_EOF){
    if(t->eof)return 0;
    if(t->line_size>1024-t->input_size||t->events==8)return S_AGAIN;
    memcpy(t->input+t->input_size,t->line,t->line_size);t->input_size+=t->line_size;
    t->line_size=0;t->eof=1;return event(t,TERM_EOF,0,0,0);
  }
  if(r->value!=TERM_TEXT||r->extra)return S_INVALID;
  if(t->eof)return S_CLOSED;
  u8 line[256],committed[512];u32 ln=t->line_size,n=0;
  memcpy(line,t->line,ln);
  const u8* text=(const u8*)r->buffer;
  if(t->mode&TERM_LINE){
    for(u32 i=0;i<r->bytes;i++){
      u8 ch=text[i];
      if(ch==8||ch==127){if(ln){ln--;while(ln&&(line[ln]&0xc0)==0x80)ln--;}continue;}
      if(ln==256)return S_LIMIT;
      line[ln++]=ch;
      if(ch=='\n'){memcpy(committed+n,line,ln);n+=ln;ln=0;}
    }
  }else{n=r->bytes;memcpy(committed,text,n);}
  if(n>1024-t->input_size||((t->mode&TERM_ECHO)&&r->bytes>1024-t->output_size))return S_AGAIN;
  memcpy(t->input+t->input_size,committed,n);t->input_size+=n;
  memcpy(t->line,line,ln);t->line_size=ln;
  if(t->mode&TERM_ECHO){memcpy(t->output+t->output_size,text,r->bytes);t->output_size+=r->bytes;}
  return 0;
}
int terminal_call(Handle* owner,u32 pid,u32 policy,const TerminalRequest* r,FileAccess access){
  if(r->op<TERM_CREATE||r->op>TERM_READY)return S_INVALID;
  if(r->op==TERM_CREATE){
    if(policy==PROCESS_STRICT)return S_DENIED;
    if(r->handle||r->buffer||r->bytes)return S_INVALID;
    return create(owner,r->value,r->extra);
  }
  if(r->op==TERM_CURRENT){
    if(r->handle||r->buffer||r->bytes||r->value||r->extra)return S_INVALID;
    for(u32 i=0;i<HANDLES;i++)if(owner[i].generation){u32 h=(owner[i].cookie<<8)|(i+1);if(client(owner,h,pid))return (int)h;}
    return S_NOTFOUND;
  }
  Object* control=handle_get(owner,r->handle,OBJ_TERM_CONTROL,1);
  Client* c=client(owner,r->handle,pid);
  Terminal* t=control?(Terminal*)control->value:c?c->terminal:0;
  if(!t)return S_BADHANDLE;
  if(r->op==TERM_FOREGROUND){
    if(!control)return S_BADHANDLE;
    if(r->buffer||r->bytes||r->extra)return S_INVALID;
    u32 child;Handle* handles=process_terminal_target(owner,r->value,&child);
    return handles?attach(t,handles,child):S_BADHANDLE;
  }
  if(r->op==TERM_FEED){
    if(!control)return S_BADHANDLE;
    if(r->bytes>256||(r->bytes&&(!access(r->buffer,r->bytes,0)||r->value!=TERM_TEXT)))return S_INVALID;
    if(r->value!=TERM_TEXT && r->buffer)return S_INVALID;
    if(r->value!=TERM_KEY && r->value!=TERM_RESIZE && r->extra)return S_INVALID;
    return feed(t,r);
  }
  if(r->op==TERM_INFO){
    if(r->value||r->extra||r->bytes<sizeof(TerminalInfo)||!access(r->buffer,sizeof(TerminalInfo),1))return S_INVALID;
    TerminalInfo info={sizeof(info),1,t->width,t->height,t->mode,t->foreground,t->closed,0};
    memcpy((void*)r->buffer,&info,sizeof(info));return 0;
  }
  if(r->op==TERM_DRAIN){
    if(!control)return S_BADHANDLE;
  }else{
    if(!c)return S_BADHANDLE;
    if(t->foreground!=pid)return S_DENIED;
  }
  if(r->op==TERM_MODE){
    if(r->buffer||r->bytes||r->extra||(r->value&~3u))return S_INVALID;
    if(t->closed)return S_CLOSED;
    if(t->line_size)return S_BUSY; /* Never silently discard a partial line. */
    t->mode=r->value;return 0;
  }
  if(r->value||r->extra)return S_INVALID;
  if(r->op==TERM_READY){
    if(r->buffer||r->bytes)return S_INVALID;
    return (t->input_size||(!t->closed&&t->eof)?STREAM_READABLE:0)|
      (!t->closed&&t->output_size<1024?STREAM_WRITABLE:0)|
      (!t->closed&&t->eof?STREAM_EOF:0)|(t->closed?STREAM_HANGUP:0);
  }
  if(r->op==TERM_EVENT){
    if(r->bytes<sizeof(TerminalEvent)||!access(r->buffer,sizeof(TerminalEvent),1))return S_INVALID;
    if(!t->events)return t->closed?S_CLOSED:S_AGAIN;
    memcpy((void*)r->buffer,&t->event[0],sizeof(TerminalEvent));t->events--;
    for(u32 i=0;i<t->events;i++)t->event[i]=t->event[i+1];return 0;
  }
  if(r->op!=TERM_READ&&r->op!=TERM_WRITE&&r->op!=TERM_DRAIN)return S_INVALID;
  if(r->bytes>1024||(r->bytes&&!access(r->buffer,r->bytes,r->op!=TERM_WRITE)))return S_INVALID;
  if(!r->bytes)return 0;
  if(r->op==TERM_WRITE){
    if(t->closed)return S_CLOSED;
    u32 n=1024-t->output_size;if(n>r->bytes)n=r->bytes;
    if(!n)return S_AGAIN;
    memcpy(t->output+t->output_size,(void*)r->buffer,n);t->output_size+=n;return (int)n;
  }
  if(r->op==TERM_DRAIN)return t->output_size?pop(t->output,&t->output_size,(void*)r->buffer,r->bytes):t->closed?0:S_AGAIN;
  if(t->input_size)return pop(t->input,&t->input_size,(void*)r->buffer,r->bytes);
  return t->closed?S_CLOSED:t->eof?0:S_AGAIN;
}
