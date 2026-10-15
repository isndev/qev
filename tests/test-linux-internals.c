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
 * loop's recorded realtime-monotonic offset, which is what a jump changes.
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

    printf("\n== linux-internals: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);
    /* Four checks; they skip only on a kernel that refuses timerfd_create (it exists since 2.6.25).
       A passing run below four measured less than it claims. */
    if (!g_fail && !g_skip && g_run < 4) {
        printf("== FAIL: only %d checks ran; 4 are expected ==\n", g_run);
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
