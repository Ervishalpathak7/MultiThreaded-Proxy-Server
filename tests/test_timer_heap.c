#include "core/timer_heap.h"

#include "core/event_loop.h"
#include "test.h"

/* Each fired timer appends its id to this log so we can check ordering. */
static int fired[64];
static int nfired;

static void record(timer_t_ *t)
{
    fired[nfired++] = (int)(intptr_t)t->arg;
}

TEST(fires_in_deadline_order)
{
    timer_heap_t h;
    timer_heap_init(&h);
    timer_t_ t[5];
    uint64_t deadlines[5] = {50, 10, 40, 20, 30};
    for (int i = 0; i < 5; i++) {
        timer_init(&t[i], record, (void *)(intptr_t)i);
        timer_schedule(&h, &t[i], deadlines[i]);
    }
    ASSERT_EQ(timer_heap_next_ms(&h, 0), 10);
    ASSERT_EQ(timer_heap_next_ms(&h, 15), 0);

    nfired = 0;
    timer_heap_run(&h, 100);
    ASSERT_EQ(nfired, 5);
    /* Expected order by deadline: t1(10) t3(20) t4(30) t2(40) t0(50) */
    ASSERT_EQ(fired[0], 1);
    ASSERT_EQ(fired[1], 3);
    ASSERT_EQ(fired[2], 4);
    ASSERT_EQ(fired[3], 2);
    ASSERT_EQ(fired[4], 0);
    ASSERT_EQ(timer_heap_next_ms(&h, 0), -1);
    for (int i = 0; i < 5; i++)
        ASSERT(!timer_is_scheduled(&t[i]));
    timer_heap_free(&h);
}

TEST(only_due_timers_fire)
{
    timer_heap_t h;
    timer_heap_init(&h);
    timer_t_ a, b;
    timer_init(&a, record, (void *)1);
    timer_init(&b, record, (void *)2);
    timer_schedule(&h, &a, 100);
    timer_schedule(&h, &b, 200);
    nfired = 0;
    timer_heap_run(&h, 150);
    ASSERT_EQ(nfired, 1);
    ASSERT_EQ(fired[0], 1);
    ASSERT(timer_is_scheduled(&b));
    timer_heap_free(&h);
}

TEST(cancel_removes_from_middle)
{
    timer_heap_t h;
    timer_heap_init(&h);
    timer_t_ t[10];
    for (int i = 0; i < 10; i++) {
        timer_init(&t[i], record, (void *)(intptr_t)i);
        timer_schedule(&h, &t[i], (uint64_t)(i * 10));
    }
    timer_cancel(&h, &t[4]);
    timer_cancel(&h, &t[7]);
    timer_cancel(&h, &t[7]); /* double cancel must be harmless */
    ASSERT(!timer_is_scheduled(&t[4]));
    nfired = 0;
    timer_heap_run(&h, 1000);
    ASSERT_EQ(nfired, 8);
    int prev = -1;
    for (int i = 0; i < nfired; i++) {
        ASSERT(fired[i] != 4 && fired[i] != 7);
        ASSERT(fired[i] > prev); /* still sorted */
        prev = fired[i];
    }
    timer_heap_free(&h);
}

TEST(reschedule_moves_deadline)
{
    timer_heap_t h;
    timer_heap_init(&h);
    timer_t_ a, b;
    timer_init(&a, record, (void *)1);
    timer_init(&b, record, (void *)2);
    timer_schedule(&h, &a, 100);
    timer_schedule(&h, &b, 200);
    /* Push `a` later and pull `b` earlier; order must flip. */
    timer_schedule(&h, &a, 300);
    timer_schedule(&h, &b, 50);
    nfired = 0;
    timer_heap_run(&h, 1000);
    ASSERT_EQ(fired[0], 2);
    ASSERT_EQ(fired[1], 1);
    timer_heap_free(&h);
}

/* Timers driven through the event loop end-to-end. */
static void stop_loop(timer_t_ *t)
{
    record(t);
    ev_stop((event_loop_t *)t->arg);
}

TEST(event_loop_runs_timers)
{
    event_loop_t *loop = ev_new();
    ASSERT(loop != NULL);
    timer_t_ t;
    timer_init(&t, stop_loop, loop);
    ev_timer_after(loop, &t, 20);
    uint64_t start = ev_now(loop);
    nfired = 0;
    ev_run(loop); /* returns once the timer calls ev_stop */
    ASSERT_EQ(nfired, 1);
    ASSERT(ev_now(loop) - start >= 20);
    ev_free(loop);
}

int main(void)
{
    RUN(fires_in_deadline_order);
    RUN(only_due_timers_fire);
    RUN(cancel_removes_from_middle);
    RUN(reschedule_moves_deadline);
    RUN(event_loop_runs_timers);
    return TEST_RESULT();
}
