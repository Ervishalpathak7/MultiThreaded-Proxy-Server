/*
 * timer_heap.c - intrusive binary min-heap (see timer_heap.h).
 */
#include "core/timer_heap.h"

#include <stdlib.h>

#include "core/util.h"

void timer_heap_init(timer_heap_t *h)
{
    h->items = NULL;
    h->len = h->cap = 0;
}

void timer_heap_free(timer_heap_t *h)
{
    for (size_t i = 0; i < h->len; i++)
        h->items[i]->idx = TIMER_NOT_SCHEDULED;
    free(h->items);
    timer_heap_init(h);
}

void timer_init(timer_t_ *t, timer_cb cb, void *arg)
{
    t->deadline = 0;
    t->cb = cb;
    t->arg = arg;
    t->idx = TIMER_NOT_SCHEDULED;
}

/* Swap two heap slots and keep the back-pointers in sync. */
static void swap(timer_heap_t *h, size_t a, size_t b)
{
    timer_t_ *tmp = h->items[a];
    h->items[a] = h->items[b];
    h->items[b] = tmp;
    h->items[a]->idx = a;
    h->items[b]->idx = b;
}

static void sift_up(timer_heap_t *h, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (h->items[parent]->deadline <= h->items[i]->deadline)
            break;
        swap(h, i, parent);
        i = parent;
    }
}

static void sift_down(timer_heap_t *h, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, smallest = i;
        if (l < h->len && h->items[l]->deadline < h->items[smallest]->deadline)
            smallest = l;
        if (r < h->len && h->items[r]->deadline < h->items[smallest]->deadline)
            smallest = r;
        if (smallest == i)
            break;
        swap(h, i, smallest);
        i = smallest;
    }
}

/* Remove slot i: move the last element into it and restore the invariant. */
static void remove_at(timer_heap_t *h, size_t i)
{
    h->items[i]->idx = TIMER_NOT_SCHEDULED;
    h->len--;
    if (i == h->len)
        return;
    h->items[i] = h->items[h->len];
    h->items[i]->idx = i;
    /* The moved element may need to go either direction. */
    sift_up(h, i);
    sift_down(h, h->items[i]->idx);
}

void timer_schedule(timer_heap_t *h, timer_t_ *t, uint64_t deadline)
{
    if (timer_is_scheduled(t)) {
        /* Rescheduling in place avoids a remove + insert pair. */
        uint64_t old = t->deadline;
        t->deadline = deadline;
        if (deadline < old)
            sift_up(h, t->idx);
        else
            sift_down(h, t->idx);
        return;
    }
    if (h->len == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->items = xrealloc(h->items, h->cap * sizeof(*h->items));
    }
    t->deadline = deadline;
    t->idx = h->len;
    h->items[h->len++] = t;
    sift_up(h, t->idx);
}

void timer_cancel(timer_heap_t *h, timer_t_ *t)
{
    if (!timer_is_scheduled(t))
        return;
    remove_at(h, t->idx);
}

int64_t timer_heap_next_ms(const timer_heap_t *h, uint64_t now)
{
    if (h->len == 0)
        return -1;
    uint64_t d = h->items[0]->deadline;
    return d <= now ? 0 : (int64_t)(d - now);
}

void timer_heap_run(timer_heap_t *h, uint64_t now)
{
    while (h->len > 0 && h->items[0]->deadline <= now) {
        timer_t_ *t = h->items[0];
        remove_at(h, 0);
        /* The callback may free `t`'s owner, so touch nothing afterwards. */
        t->cb(t);
    }
}
