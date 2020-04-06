/*
 * Copyright (c) 2020, Yanhui Shi <lime.syh at gmail dot com>
 * All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "common.h"
#include "core.h"
#include "mutex.h"
#include "proc.h"
#include "timer.h"

#define csp_timer_getclock() ({                                                \
  uint32_t high, low;                                                          \
  __asm__ __volatile__("rdtsc\n": "=d"(high), "=a"(low));                      \
  ((int64_t)high << 32) | low;                                                 \
})                                                                             \

#define csp_timer_heap_default_cap 64
#define csp_timer_heap_lte(heap, i, j)                                         \
  ((heap)->procs[i]->timer.when <= (heap)->procs[j]->timer.when)

/* Only for debug. */
#define csp_timer_heap_dump(heap) do {                                         \
  for (int i = 0; i < (heap)->len; i++) {                                      \
    printf(                                                                    \
      "<csp_proc_t %p, rbp: %lx, rsp: %lx, "                                   \
      "idx: %ld, when: %ld, token: %lx>\n",                                    \
      (heap)->procs[i],                                                        \
      (heap)->procs[i]->rbp,                                                   \
      (heap)->procs[i]->rsp,                                                   \
      (heap)->procs[i]->timer.idx,                                             \
      (heap)->procs[i]->timer.when,                                            \
      (heap)->procs[i]->timer.token                                            \
    );                                                                         \
  }                                                                            \
} while (0)                                                                    \

extern int csp_sched_np;
extern _Thread_local csp_core_t *csp_this_core;

extern void csp_core_proc_exit(void);
extern void csp_proc_destroy(csp_proc_t *proc);
extern void csp_sched_yield(void);

typedef struct csp_timer_heap_t {
  size_t cap, len;
  csp_proc_t **procs;
  csp_timer_time_t time, clock;
  int64_t token;
  csp_mutex_t mutex;
} csp_timer_heap_t;

bool csp_timer_heap_init(csp_timer_heap_t *heap, size_t pid) {
  heap->cap = csp_timer_heap_default_cap;
  heap->len = 0;
  heap->time = csp_timer_now();
  heap->clock = csp_timer_getclock();
  heap->procs = (csp_proc_t **)malloc(sizeof(csp_proc_t *) * heap->cap);

  /* Make tokens generated by different `csp_timer_heap_t` different. */
  heap->token = (uint64_t)pid << 53;

  csp_mutex_init(&heap->mutex);
  return heap->procs != NULL;
}

/* Put a timer to the heap. */
void csp_timer_heap_put(csp_timer_heap_t *heap, csp_proc_t *proc) {
  csp_mutex_lock(&heap->mutex);

  /* Grow the heap if it's not large enough. */
  if (csp_unlikely(heap->len == heap->cap)) {
    size_t cap = heap->cap << 1;
    csp_proc_t **procs = (csp_proc_t **)realloc(heap->procs, cap);
    if (csp_unlikely(procs == NULL)) {
      exit(EXIT_FAILURE);
    }
    heap->procs = procs;
    heap->cap = cap;
  }

  csp_proc_timer_token_set(proc, heap->token);
  heap->token++;

  heap->procs[heap->len] = proc;
  proc->timer.idx = heap->len++;

  int64_t son = proc->timer.idx, father;
  while (son > 0) {
    father = (son - 1) >> 1;
    if (csp_timer_heap_lte(heap, father, son)) {
      break;
    }
    csp_swap(heap->procs[son], heap->procs[father]);
    csp_swap(heap->procs[son]->timer.idx, heap->procs[father]->timer.idx);
    son = father;
  }

  csp_mutex_unlock(&heap->mutex);
}

/* Delete a timer from the heap. The caller should take control of the mutex. */
void csp_timer_heap_del(csp_timer_heap_t *heap, csp_proc_t *proc) {
  int64_t father = proc->timer.idx;
  if (father == --heap->len) {
    return;
  }

  heap->procs[father] = heap->procs[heap->len];
  heap->procs[father]->timer.idx = father;

  while (true) {
    int64_t son = (father << 1) + 1;
    if (son >= heap->len) {
      break;
    }
    if (son + 1 < heap->len && csp_timer_heap_lte(heap, son + 1, son)) {
      son++;
    }
    if (csp_timer_heap_lte(heap, father, son)) {
      break;
    }
    csp_swap(heap->procs[father], heap->procs[son]);
    csp_swap(heap->procs[father]->timer.idx, heap->procs[son]->timer.idx);
    father = son;
  }
}

/* Get all expired timers from the heap. */
static int csp_timer_heap_get(csp_timer_heap_t *heap, csp_proc_t **start,
    csp_proc_t **end) {
  csp_mutex_lock(&heap->mutex);

  if (heap->len == 0) {
    csp_mutex_unlock(&heap->mutex);
    return 0;
  }

  csp_timer_time_t clock = csp_timer_getclock(), curr_time;
  csp_timer_duration_t duration = clock - heap->clock;

  /* Use the approximation calculated by clock instead of the real time to
   * reduce the syscall calls. */
  if (duration < CLOCKS_PER_SEC) {
    curr_time = heap->time + (csp_timer_duration_t)(
      ((double)duration / CLOCKS_PER_SEC) * csp_timer_second
    );
  } else {
    heap->clock = clock;
    curr_time = heap->time = csp_timer_now();
  }

  int n = 0;
  csp_proc_t *head = NULL, *tail = NULL, *top;

  while (heap->len > 0 && (top = heap->procs[0])->timer.when <= curr_time) {
    csp_timer_heap_del(heap, top);
    /*  Invalidate the token. */
    csp_proc_timer_token_set(top, -1);

    if (tail == NULL) {
      head = tail = top;
    } else {
      tail->next = top;
      top->pre = tail;
      tail = top;
    }
    n++;
  }

  if (n > 0) {
    *start = head;
    *end = tail;
  }

  csp_mutex_unlock(&heap->mutex);
  return n;
}

void csp_timer_heap_destroy(csp_timer_heap_t *heap) {
  free(heap->procs);
}

struct { int len; csp_timer_heap_t *heaps; } csp_timer_heaps;

bool csp_timer_heaps_init(void) {
  csp_timer_heaps.heaps = (csp_timer_heap_t *)malloc(
    sizeof(csp_timer_heap_t) * csp_sched_np
  );
  if (csp_timer_heaps.heaps == NULL) {
    return false;
  }

  for (int i = 0; i < csp_sched_np; i++) {
    if(!csp_timer_heap_init(&csp_timer_heaps.heaps[i], i)) {
      csp_timer_heaps.len = i;
      return false;
    }
  }
  csp_timer_heaps.len = csp_sched_np;
  return true;
}

void csp_timer_heaps_destroy(void) {
  for (size_t i = 0; i < csp_timer_heaps.len; i++) {
    csp_timer_heap_destroy(&csp_timer_heaps.heaps[i]);
  }
  free(csp_timer_heaps.heaps);
}

void csp_timer_put(size_t pid, csp_proc_t *proc) {
  csp_timer_heap_put(&csp_timer_heaps.heaps[pid], proc);
}

/* Poll all expired timers from all heaps. */
int csp_timer_poll(csp_proc_t **start, csp_proc_t **end) {
  int total = 0;
  csp_proc_t *head, *tail;

  for (int i = 0; i < csp_timer_heaps.len; i++) {
    int n = csp_timer_heap_get(&csp_timer_heaps.heaps[i], &head, &tail);
    if (n > 0) {
      if (total != 0) {
        (*end)->next = head;
        head->pre = *end;
        *end = tail;
      } else {
        *start = head;
        *end = tail;
      }
      total += n;
    }
  }
  return total;
}

bool csp_timer_cancel(csp_timer_t timer) {
  csp_timer_heap_t *heap = &csp_timer_heaps.heaps[timer.ctx->borned_pid];

  csp_mutex_lock(&heap->mutex);
  /* Check whether the token is valid. */
  if (!csp_proc_timer_token_cas(timer.ctx, timer.token, -1)) {
    csp_mutex_unlock(&heap->mutex);
    return false;
  }

  csp_timer_heap_del(heap, timer.ctx);
  csp_mutex_unlock(&heap->mutex);
  csp_proc_destroy(timer.ctx);
  return true;
}

csp_proc void csp_timer_anchor(csp_timer_time_t when) {};
