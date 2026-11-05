#ifndef SINO_UI_QUEUE_H
#define SINO_UI_QUEUE_H
#include "api.h"
typedef struct {
  UiEvent items[16], close_event, cancel_event;
  u32 head, tail, lost, close, cancel;
} UiQueue;
static void ui_queue_push(UiQueue *q, UiEvent e, int priority) {
  if (priority && e.type == MSG_CLOSE) {
    q->close_event = e;
    q->close = 1;
    return;
  }
  if (priority && e.type == MSG_KEY && e.key == 3) {
    q->cancel_event = e;
    q->cancel = 1;
    q->tail = q->head;
    return;
  }
  if (e.type == MSG_FILES_CHANGED)
    for (u32 i = q->tail; i != q->head; i = (i + 1) & 15)
      if (q->items[i].type == e.type && q->items[i].window == e.window)
        return;
  u32 last = (q->head - 1) & 15;
  if (e.type == MSG_MOUSE_MOVE && q->head != q->tail &&
      q->items[last].type == MSG_MOUSE_MOVE) {
    q->items[last] = e;
    return;
  }
  u32 next = (q->head + 1) & 15;
  if (next == q->tail) {
    if (priority && (e.type == MSG_KEY || e.type == MSG_TEXT) &&
        q->lost != 0xffffffffu)
      q->lost++;
    return;
  }
  q->items[q->head] = e;
  q->head = next;
}
static int ui_queue_pop(UiQueue *q, UiEvent *e) {
  if (q->close) {
    q->close = 0;
    *e = q->close_event;
    return 0;
  }
  if (q->cancel) {
    q->cancel = 0;
    *e = q->cancel_event;
    return 0;
  }
  if (q->lost) {
    *e = (UiEvent){MSG_INPUT_REJECTED, 0, 0, 0, (i32)q->lost};
    q->lost = 0;
    q->tail = q->head;
    return 0;
  }
  if (q->head == q->tail)
    return S_AGAIN;
  *e = q->items[q->tail];
  q->tail = (q->tail + 1) & 15;
  return 0;
}
#endif
