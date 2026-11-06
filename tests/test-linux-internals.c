/*
 * qev — white-box test of Linux-only loop paths (Linux only).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The realtime timerfd (EV_USE_TIMERFD, off by default in qev and qb) wakes the loop when the wall
 * clock is set (TFD_TIMER_CANCEL_ON_SET) -- and also when its own deadline, MAX_BLOCKTIME2 (about
 * seventeen days) ahead, runs out, and once after a fork, where loop_fork feeds it. Its callback
 * used to recompute every periodic on each of those wakes: an interval periodic due but not yet
 * delivered was pushed past its fire, and every rescheduler was asked again, with no clock change
 * at all (Huly QB-1055, the sibling of QB-316). None of those wakes can be produced on demand
 * without setting the system clock or waiting seventeen days, so this test compiles ev.c INTO
 * itself -- it links no qev -- with the timerfd forced on, and calls the callback the way the loop
 * does when the fd turns readable. A wall-clock jump is simulated the same way: by moving the
 * loop's recorded realtime-monotonic offset, which is what a jump changes. The same compile shows
 * that the timerfd is the loop's own and not an fd a non-blocking pass polls for (Huly QB-1277),
 * through a wrapper over the loop's backend poll, and that a blocking pass is still woken by it.
 *
 * Registered by CMake only on Linux with <sys/timerfd.h>.
 */
#ifdef __linux__

/* the configuration the qev target compiles ev.c with, then the timerfd forced on */
#include QB_EV_CONFIG_H
#undef EV_USE_TIMERFD
#define EV_USE_TIMERFD 1
#include "../ev.c"

#include <stdio.h>
#include <time.h>

static int g_fail = 0, g_run = 0, g_skip = 0;
#define OK(cond, name)   do { ++g_run; if (cond) printf("  PASS  %s\n", (name)); else { printf("  FAIL  %s\n", (name)); ++g_fail; } } while (0)
#define SKIP(name, why)  do { ++g_skip; printf("  SKIP  %s (%s)\n", (name), (why)); } while (0)

static void msleep(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000L * 1000L}; nanosleep(&ts, NULL); }

static int       rs_calls;
static ev_tstamp rs_cb(ev_periodic *w, ev_tstamp now) { (void) w; ++rs_calls; return now + 3600.; }
static int       per_hits;
static void      per_cb(struct ev_loop *l, ev_periodic *w, int r) { (void) l; (void) w; (void) r; ++per_hits; }

/* the timerfd's callback, as the loop invokes it when the fd is readable */
static void timerfd_wakes(struct ev_loop *l) { timerfdcb(l, &l->timerfd_w, EV_READ); }

/* A rescheduled periodic armed, then the timerfd wakes with no clock change: is the rescheduler
   asked again? With jump != 0 the loop's recorded offset is moved first, as a wall-clock set
   moves it, and the rescheduler must then be asked. Returns the calls the wake made, or -1 when
   the loop has no timerfd. */
static int reschedules_on_wake(double jump) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_periodic     p;
    int             calls;
    ev_periodic_init(&p, per_cb, 0., 0., rs_cb);
    ev_periodic_start(l, &p); /* the first periodic creates and arms the timerfd */
    if (!ev_is_active(&l->timerfd_w)) {
        ev_periodic_stop(l, &p);
        ev_loop_destroy(l);
        return -1;
    }
    ev_run(l, EVRUN_NOWAIT);
    rs_calls = 0;
    l->rtmn_diff -= jump;
    timerfd_wakes(l);
    calls = rs_calls;
    ev_periodic_stop(l, &p);
    ev_loop_destroy(l);
    return calls;
}

/* An interval periodic falls due (50 ms out, then hourly) and the timerfd wakes before the pass
   that would deliver it: does that pass still fire it? wake == 0 is the control, no wake. */
static int fires_after_wake(int wake) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_periodic     p;
    int             hits;
    ev_periodic_init(&p, per_cb, ev_now(l) + 0.05, 3600., 0);
    ev_periodic_start(l, &p);
    if (!ev_is_active(&l->timerfd_w)) {
        ev_periodic_stop(l, &p);
        ev_loop_destroy(l);
        return -1;
    }
    per_hits = 0;
    ev_run(l, EVRUN_NOWAIT); /* not due yet */
    msleep(100);             /* now it is, by the wall clock and by the loop's next reading */
    if (wake)
        timerfd_wakes(l);
    ev_run(l, EVRUN_NOWAIT);
    hits = per_hits;
    ev_periodic_stop(l, &p);
    ev_loop_destroy(l);
    return hits;
}

/* ---- the realtime timerfd is the loop's own (Huly QB-1277) ----
   The first periodic creates the timerfd, and it lives as long as the loop. It counted in iocnt --
   ev_io_count, what an embedder reads before it polls, and what ev_run reads before it skips a
   non-blocking poll -- so from the first periodic on, every NOWAIT pass polled the backend, the
   periodic long stopped. It is the loop's own, like the evpipe: a NOWAIT pass learns of a jump
   without it, reading the wall clock MIN_TIMEJUMP/2 after its last read (ev_timer_next names that
   read), and a blocking pass still polls it -- the long sleep relies on it. The polls are counted
   through a wrapper over the loop's backend_poll. */
static void (*real_poll)(struct ev_loop *, ev_tstamp);
static int   polls;
static void  counting_poll(struct ev_loop *l, ev_tstamp t) { ++polls; real_poll(l, t); }
static void  count_polls(struct ev_loop *l) { real_poll = l->backend_poll; l->backend_poll = counting_poll; polls = 0; }
static void  io_noop(struct ev_loop *l, ev_io *w, int r) { (void) l; (void) w; (void) r; }

/* A hundred NOWAIT passes over a loop holding one far periodic -- stopped first when stop != 0 --
   and one quiet pipe when fd != 0. Returns the backend polls they made (-1: no timerfd); *count
   gets ev_io_count before the passes. */
static int nowait_polls(int stop, int fd, unsigned *count) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_periodic     p;
    ev_io           w;
    int             fds[2] = {-1, -1}, n;
    ev_periodic_init(&p, per_cb, ev_now(l) + 3600., 0., 0);
    ev_periodic_start(l, &p);
    if (!ev_is_active(&l->timerfd_w)) {
        ev_periodic_stop(l, &p);
        ev_loop_destroy(l);
        return -1;
    }
    if (stop)
        ev_periodic_stop(l, &p);
    if (fd && pipe(fds) == 0) {
        ev_io_init(&w, io_noop, fds[0], EV_READ);
        ev_io_start(l, &w);
    }
    *count = ev_io_count(l);
    count_polls(l);
    for (n = 0; n < 100; ++n)
        ev_run(l, EVRUN_NOWAIT);
    n = polls;
    l->backend_poll = real_poll;
    if (fds[0] >= 0) {
        ev_io_stop(l, &w);
        close(fds[0]);
        close(fds[1]);
    }
    ev_periodic_stop(l, &p);
    ev_loop_destroy(l);
    return n;
}

/* A blocking pass over a far periodic (and a 2 s guard timer) while the timerfd turns readable
   50 ms in -- re-armed here to that deadline, the way a clock set makes it readable: is the pass
   woken by it, and its callback run? Returns the callback's runs, or -1 with no timerfd; *slept
   gets the pass's length. */
static int       tfd_hits;
static void      counting_tfd_cb(struct ev_loop *l, ev_io *w, int r) { ++tfd_hits; timerfdcb(l, w, r); }
static void      guard_cb(struct ev_loop *l, ev_timer *w, int r) { (void) l; (void) w; (void) r; }
static int blocking_pass_sees_timerfd(double *slept) {
    struct ev_loop   *l = ev_loop_new(EVFLAG_AUTO);
    ev_periodic       p;
    ev_timer          guard;
    struct itimerspec its = {{0, 0}, {0, 0}};
    struct timespec   rt;
    ev_tstamp         t0;
    int               hits;
    ev_periodic_init(&p, per_cb, ev_now(l) + 3600., 0., 0);
    ev_periodic_start(l, &p);
    if (!ev_is_active(&l->timerfd_w)) {
        ev_periodic_stop(l, &p);
        ev_loop_destroy(l);
        return -1;
    }
    ev_timer_init(&guard, guard_cb, 2., 0.);
    ev_timer_start(l, &guard);
    ev_set_cb(&l->timerfd_w, counting_tfd_cb);
    tfd_hits = 0;
    clock_gettime(CLOCK_REALTIME, &rt);
    its.it_value.tv_sec  = rt.tv_sec + (rt.tv_nsec + 50000000L) / 1000000000L;
    its.it_value.tv_nsec = (rt.tv_nsec + 50000000L) % 1000000000L;
    timerfd_settime(l->timerfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, 0);
    t0 = ev_time();
    ev_run(l, EVRUN_ONCE);
    *slept = ev_time() - t0;
    hits   = tfd_hits;
    ev_timer_stop(l, &guard);
    ev_periodic_stop(l, &p);
    ev_loop_destroy(l);
    return hits;
}

/* The pass qb's gate makes once ev_timer_next is due: the wall clock was set 10 s ahead of what the
   loop recorded (its offset moved, as in reschedules_on_wake) and its last read of it is a second
   old. Does that NOWAIT pass, handed the embedder's reading, reschedule the periodics -- without a
   backend poll, so without the timerfd? Returns the rescheduler's calls (-1: no timerfd); *due says
   whether ev_timer_next named the read before the pass, *polled counts the pass's polls and *next
   whether the next read moved half a second on. */
static int nowait_pass_sees_jump(int *due, int *polled, int *next) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_periodic     p;
    ev_tstamp       now;
    int             calls;
    ev_periodic_init(&p, per_cb, 0., 0., rs_cb);
    ev_periodic_start(l, &p);
    if (!ev_is_active(&l->timerfd_w)) {
        ev_periodic_stop(l, &p);
        ev_loop_destroy(l);
        return -1;
    }
    ev_run(l, EVRUN_NOWAIT);
    rs_calls = 0;
    l->rtmn_diff -= 10.;
    l->now_floor -= 1.;
    now  = ev_clock_now();
    *due = ev_timer_next(l) < now;
    count_polls(l);
    ev_now_set(l, now);
    ev_run(l, EVRUN_NOWAIT);
    *polled         = polls;
    l->backend_poll = real_poll;
    *next           = ev_timer_next(l) > now + 0.4;
    calls           = rs_calls;
    ev_periodic_stop(l, &p);
    ev_loop_destroy(l);
    return calls;
}

int main(void) {
    int r;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("qev Linux internals (white-box, ev %d.%d, timerfd forced on)\n", ev_version_major(), ev_version_minor());

    r = reschedules_on_wake(0.);
    if (r < 0)
        SKIP("the timerfd's wake", "timerfd_create failed: the loop runs without a timerfd");
    else {
        OK(r == 0, "the timerfd wakes with no wall-clock change: the rescheduler is not asked again (QB-1055)");
        OK(reschedules_on_wake(10.) == 1, "control: the timerfd wakes after a 10 s wall-clock jump: the periodics are recomputed");
        OK(fires_after_wake(0) == 1, "control: an interval periodic due 50 ms in fires on the pass after its deadline");
        OK(fires_after_wake(1) == 1, "the same periodic, with the timerfd waking before that pass and no clock change: it still fires (QB-1055)");
    }

    {
        unsigned count = 0;
        r = nowait_polls(0, 0, &count);
        if (r < 0)
            SKIP("the timerfd is the loop's own", "timerfd_create failed: the loop runs without a timerfd");
        else {
            int    due = 0, polled = -1, next = 0, calls;
            double slept = 0.;
            OK(count == 0, "a periodic's timerfd is not an fd the embedder polls for: ev_io_count reads 0 (QB-1277)");
            OK(r == 0, "a hundred NOWAIT passes over a far periodic make no backend poll (QB-1277: one each)");
            OK(nowait_polls(1, 0, &count) == 0 && count == 0, "the periodic stopped, its timerfd still open: no poll and a count of 0 (QB-1277)");
            OK(nowait_polls(0, 1, &count) == 100 && count == 1, "beside a quiet pipe every NOWAIT pass still polls, and the pipe alone counts (QB-1277: the timerfd with it)");
            r = blocking_pass_sees_timerfd(&slept);
            OK(r == 1 && slept < 1.0, "a blocking pass is still woken by the timerfd turning readable, and runs its callback (50 ms in, a 2 s guard)");
            calls = nowait_pass_sees_jump(&due, &polled, &next);
            OK(due, "after a wall-clock read a second old, ev_timer_next names the next read as due (QB-1277)");
            OK(calls == 1 && polled == 0, "the NOWAIT pass made at that read sees a 10 s jump and reschedules, with no backend poll (QB-1277)");
            OK(next, "and the next read is half a second on");
        }
    }

    printf("\n== linux-internals: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);
    /* Twelve checks; they skip only on a kernel that refuses timerfd_create (it exists since 2.6.25).
       A passing run below twelve measured less than it claims. */
    if (!g_fail && !g_skip && g_run < 12) {
        printf("== FAIL: only %d checks ran; 12 are expected ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}

#else /* !__linux__ */
/* Never registered by CMake off Linux; this stub only keeps a stray compile honest. */
#include <stdio.h>
int main(void) {
    printf("the Linux internals test is Linux-only; this binary should not have been built here\n");
    return 1;
}
#endif
