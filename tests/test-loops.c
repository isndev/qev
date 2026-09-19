/*
 * qev — event-loop mechanics test.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The watcher test proves each watcher FAMILY fires; this file proves the LOOP
 * around them behaves: multi-loop isolation, timer ordering and repeat drift,
 * priorities, the README's "NULL if unavailable" backend promise, ev_now
 * bookkeeping, manual event feeding, a real fork(), an ev_async_send from a
 * second thread, and a thousand concurrent timers. Same harness discipline as
 * test-watchers.c: every loop is driven under a watchdog so a hang is a
 * failure, families the build profile removed SKIP instead of failing to
 * compile, and the suite refuses to report success when fewer than its
 * unconditional checks ran.
 */
#include <qev/ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#ifndef _WIN32
# include <pthread.h>
# include <sys/types.h>
# include <sys/wait.h>
# include <unistd.h>
# include <time.h>
static void msleep(int ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000L * 1000L }; nanosleep(&ts, NULL); }
#else
# include <windows.h>
static void msleep(int ms) { Sleep((DWORD) ms); }
#endif
#if defined(__linux__) && EV_USE_EPOLL_PWAIT2
# include <errno.h>
# include <sys/epoll.h>
#endif

static int g_fail = 0, g_run = 0, g_skip = 0;
#define OK(cond, name)   do { ++g_run; if (cond) printf("  PASS  %s\n", (name)); else { printf("  FAIL  %s\n", (name)); ++g_fail; } } while (0)
#define SKIP(name, why)  do { ++g_skip; printf("  SKIP  %s (%s)\n", (name), (why)); } while (0)

static void wd_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ev_break(l, EVBREAK_ALL); }
static void stop_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ev_break(l, EVBREAK_ALL); }

/* ---- two loops do not share watchers ---- */
static int iso_a, iso_b;
static void iso_a_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; iso_a = 1; ev_break(l, EVBREAK_ALL); }
static void iso_b_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; iso_b = 1; ev_break(l, EVBREAK_ALL); }
static void test_multi_loop_isolation(void) {
    iso_a = iso_b = 0;
    struct ev_loop *a = ev_loop_new(EVFLAG_AUTO);
    struct ev_loop *b = ev_loop_new(EVFLAG_AUTO);
    ev_timer ta; ev_timer_init(&ta, iso_a_cb, 0.01, 0.0); ev_timer_start(a, &ta);
    ev_timer tb; ev_timer_init(&tb, iso_b_cb, 3600.0, 0.0); ev_timer_start(b, &tb);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(a, &wd);
    ev_run(a, 0);
    OK(iso_a == 1 && iso_b == 0, "multi-loop isolation (running A never fires B's watchers)");
    ev_timer_stop(b, &tb);
    ev_loop_destroy(a);
    ev_loop_destroy(b);
}

/* ---- expiry order follows deadlines ---- */
static int order_seq[4], order_n;
static void order_cb1(struct ev_loop *l, ev_timer *w, int r) { (void)l; (void)w; (void)r; if (order_n < 4) order_seq[order_n++] = 1; }
static void order_cb2(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; if (order_n < 4) order_seq[order_n++] = 2; ev_break(l, EVBREAK_ALL); }
static void test_timer_ordering(void) {
    order_n = 0; memset(order_seq, 0, sizeof(order_seq));
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t2; ev_timer_init(&t2, order_cb2, 0.06, 0.0); ev_timer_start(l, &t2);
    ev_timer t1; ev_timer_init(&t1, order_cb1, 0.01, 0.0); ev_timer_start(l, &t1);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(order_n == 2 && order_seq[0] == 1 && order_seq[1] == 2,
       "timer expiry order follows deadlines, not start order");
    ev_loop_destroy(l);
}

/* ---- a repeating timer neither stalls nor runs away ---- */
static int    rep_count;
static double rep_t0;
static void rep_cb(struct ev_loop *l, ev_timer *w, int r) {
    (void)w; (void)r;
    if (++rep_count >= 5) ev_break(l, EVBREAK_ALL);
}
static void test_timer_repeat(void) {
    rep_count = 0;
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    rep_t0 = ev_time();
    ev_timer t; ev_timer_init(&t, rep_cb, 0.01, 0.01); ev_timer_start(l, &t);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 5.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    double elapsed = ev_time() - rep_t0;
    /* 5 ticks of 10ms: >= 40ms proves it did not fire in a burst, and the watchdog
       already bounds the other side. The lower bound is the assertion that matters --
       a timer that fires immediately five times is broken in a way wall-clock load
       cannot excuse. */
    OK(rep_count == 5 && elapsed >= 0.04, "repeating timer paces its ticks (5 x 10ms took >= 40ms)");
    ev_timer_stop(l, &t);
    ev_loop_destroy(l);
}

/* ---- ev_timer_again re-arms from the repeat value ---- */
static int again_hits;
static void again_cb(struct ev_loop *l, ev_timer *w, int r) {
    (void)r;
    if (++again_hits == 1) { ev_timer_again(l, w); }  /* re-arm once */
    else ev_break(l, EVBREAK_ALL);
}
static void test_timer_again(void) {
    again_hits = 0;
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t; ev_timer_init(&t, again_cb, 0.01, 0.01); ev_timer_start(l, &t);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(again_hits == 2, "ev_timer_again re-arms from the repeat interval");
    ev_timer_stop(l, &t);
    ev_loop_destroy(l);
}

/* ---- pending callbacks are invoked in priority order ---- */
static int prio_seq[2], prio_n;
static void prio_hi_cb(struct ev_loop *l, ev_timer *w, int r) { (void)l; (void)w; (void)r; if (prio_n < 2) prio_seq[prio_n++] = 1; }
static void prio_lo_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; if (prio_n < 2) prio_seq[prio_n++] = 2; ev_break(l, EVBREAK_ALL); }
static void test_priorities(void) {
    prio_n = 0;
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    /* Same deadline; started low-priority FIRST so start order argues against us. */
    ev_timer lo; ev_timer_init(&lo, prio_lo_cb, 0.02, 0.0); ev_set_priority(&lo, EV_MINPRI); ev_timer_start(l, &lo);
    ev_timer hi; ev_timer_init(&hi, prio_hi_cb, 0.02, 0.0); ev_set_priority(&hi, EV_MAXPRI); ev_timer_start(l, &hi);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(prio_n == 2 && prio_seq[0] == 1 && prio_seq[1] == 2,
       "same-tick callbacks run highest priority first");
    ev_loop_destroy(l);
}

/* ---- the README's promise: ev_loop_new(unavailable backend) == NULL ---- */
static void test_backend_unavailable(void) {
    unsigned int have = ev_supported_backends();
    unsigned int all[] = {EVBACKEND_SELECT, EVBACKEND_POLL, EVBACKEND_EPOLL,
                          EVBACKEND_KQUEUE, EVBACKEND_DEVPOLL, EVBACKEND_PORT};
    unsigned int absent = 0;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        if (!(have & all[i])) { absent = all[i]; break; }
    if (!absent) { SKIP("unavailable backend returns NULL", "every probed backend is supported here"); return; }
    struct ev_loop *l = ev_loop_new(absent);
    OK(l == NULL, "ev_loop_new(unsupported backend) returns NULL, not a fallback");
    if (l) ev_loop_destroy(l);
}

/* ---- ev_now moves only when the loop updates it ---- */
static void test_now_update(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_tstamp before = ev_now(l);
    ev_timer wd; ev_timer_init(&wd, stop_cb, 0.03, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    ev_tstamp after = ev_now(l);
    OK(after > before, "ev_now advances across a blocking run");
    ev_loop_destroy(l);
}

/* ---- ev_iteration counts loop turns ---- */
static void test_iteration_count(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    unsigned int before = ev_iteration(l);
    ev_timer wd; ev_timer_init(&wd, stop_cb, 0.02, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(ev_iteration(l) > before, "ev_iteration increments per loop turn");
    ev_loop_destroy(l);
}

/* ---- ev_active_count follows start / unref / stop / one-shot expiry ---- */
static void test_active_count(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    OK(ev_active_count(l) == 0 && ev_pending_count(l) == 0, "fresh loop: no active, no pending watcher");
    ev_timer a, b;
    ev_timer_init(&a, stop_cb, 3600.0, 0.0); ev_timer_start(l, &a);
    ev_timer_init(&b, stop_cb, 0.001, 0.0);  ev_timer_start(l, &b);
    OK(ev_active_count(l) == 2, "two started timers: ev_active_count == 2");
    ev_unref(l);
    OK(ev_active_count(l) == 1, "ev_unref lowers it: an unreferenced watcher does not keep the loop alive");
    ev_ref(l);
    ev_run(l, 0);                         /* b (1 ms, one-shot) fires, auto-stops and breaks the loop */
    OK(ev_active_count(l) == 1, "a one-shot timer that fired is no longer active");
    ev_timer_stop(l, &a);
    OK(ev_active_count(l) == 0, "stopping the last watcher brings it back to 0");
    ev_feed_event(l, &a, EV_CUSTOM);      /* pending on an INACTIVE watcher is still pending */
    OK(ev_pending_count(l) == 1 && ev_active_count(l) == 0, "a fed event on a stopped watcher is pending, not active");
    ev_clear_pending(l, &a);
    ev_loop_destroy(l);
}

/* ---- ev_active_count_addr / ev_pending_count_addr alias the counters for the loop's life ---- */
static void test_count_addr(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    const int *active  = ev_active_count_addr(l);
    const int *pending = ev_pending_count_addr(l);
    int pri, sum;
    OK(active && pending && *active == 0, "fresh loop: both addresses are valid and active reads 0");
    ev_timer a, b;
    ev_timer_init(&a, stop_cb, 3600.0, 0.0); ev_timer_start(l, &a);
    ev_timer_init(&b, stop_cb, 3600.0, 0.0); ev_timer_start(l, &b);
    OK(*active == 2 && (unsigned) *active == ev_active_count(l), "two started timers: *active == 2 == ev_active_count");
    ev_feed_event(l, &a, EV_CUSTOM);
    for (sum = 0, pri = 0; pri < EV_NUMPRI; ++pri) sum += pending[pri];
    OK(sum == 1 && (unsigned) sum == ev_pending_count(l), "a fed event: the EV_NUMPRI entries sum to ev_pending_count == 1");
    ev_run(l, EVRUN_NOWAIT);              /* delivers it (stop_cb breaks); the addresses survive the run */
    for (sum = 0, pri = 0; pri < EV_NUMPRI; ++pri) sum += pending[pri];
    OK(sum == 0 && *active == 2, "after the run: nothing pending, both timers still active, same addresses");
    ev_timer_stop(l, &a); ev_timer_stop(l, &b);
    ev_unref(l);
    OK(*active == -1 && ev_active_count(l) == 0, "an unpaired ev_unref reads -1 raw where ev_active_count clamps to 0");
    ev_ref(l);
    OK(*active == 0, "ev_ref restores 0");
    ev_loop_destroy(l);
}

/* ---- ev_io_count: the fd watchers a backend poll is for; none -> a poll that would not block is skipped ---- */
static int tick_fired;
static void tick_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ++tick_fired; ev_break(l, EVBREAK_ALL); }
#ifndef _WIN32
static void io_noop_cb(struct ev_loop *l, ev_io *w, int r) { (void)l; (void)w; (void)r; }
#endif
static void test_io_count(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t;
    OK(ev_io_count(l) == 0, "fresh loop: no ev_io watcher");
    ev_timer_init(&t, tick_cb, 0.0, 0.0); ev_timer_start(l, &t);   /* already expired when the run begins */
    OK(ev_io_count(l) == 0 && ev_active_count(l) == 1, "a timer is not an ev_io: io_count stays 0 while it is active");
    tick_fired = 0;
    ev_run(l, EVRUN_NOWAIT);              /* no fd and no wait: the backend poll is skipped -- the timer must fire anyway */
    OK(tick_fired == 1, "a timers-only EVRUN_NOWAIT pass fires the expired timer without a backend poll");
    {                                     /* a blocking wait with no fd IS the sleep: kept, not skipped */
        ev_tstamp t0 = ev_time();
        ev_timer_init(&t, tick_cb, 0.005, 0.0); ev_timer_start(l, &t);
        tick_fired = 0;
        /* ONE blocking pass normally fires it, but a wait of whole milliseconds against a finer clock can
           return a hair before the deadline and leave the timer for the next pass (seen once on a hosted
           Windows VM): what is asserted is the sleep, not the wait's rounding -- so pass until it fired. */
        do ev_run(l, EVRUN_ONCE); while (tick_fired == 0 && ev_time() - t0 < 1.0);
        OK(tick_fired == 1 && ev_time() - t0 >= 0.004, "a timers-only blocking run still sleeps until the timer fires");
    }
#ifndef _WIN32
    {
        int   fds[2];
        ev_io w;
        if (pipe(fds) == 0) {
            ev_io_init(&w, io_noop_cb, fds[0], EV_READ); ev_io_start(l, &w);
            OK(ev_io_count(l) == 1, "a started ev_io counts");
            ev_io_start(l, &w);
            OK(ev_io_count(l) == 1, "starting it a second time counts it once");
            ev_io_stop(l, &w);
            OK(ev_io_count(l) == 0, "stopping it brings the count back to 0");
            ev_io_stop(l, &w);
            OK(ev_io_count(l) == 0, "stopping it a second time does not go below 0");
            close(fds[0]); close(fds[1]);
        } else {
            SKIP("ev_io_count follows ev_io_start / ev_io_stop", "pipe() failed");
        }
    }
#endif
    ev_loop_destroy(l);
}

/* ---- ev_feed_event delivers without any real readiness ---- */
static int fed;
static void fed_cb(struct ev_loop *l, ev_timer *w, int r) {
    (void)w;
    if (r & EV_CUSTOM) { fed = 1; ev_break(l, EVBREAK_ALL); }
    else ev_break(l, EVBREAK_ALL); /* the 3600s deadline would be a hang without the feed */
}
static void test_feed_event(void) {
    fed = 0;
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t; ev_timer_init(&t, fed_cb, 3600.0, 0.0); ev_timer_start(l, &t);
    ev_feed_event(l, &t, EV_CUSTOM);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(fed == 1, "ev_feed_event delivers EV_CUSTOM with no real readiness");
    ev_timer_stop(l, &t);
    ev_loop_destroy(l);
}

/* ---- stopping a never-started watcher is a safe no-op ---- */
static void test_stop_inactive(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t; ev_timer_init(&t, wd_cb, 1.0, 0.0);
    ev_timer_stop(l, &t); /* never started */
    OK(!ev_is_active(&t), "ev_timer_stop on a never-started watcher is a no-op");
    ev_loop_destroy(l);
}

/* ---- a thousand timers all fire ---- */
static int  many_fired;
static void many_cb(struct ev_loop *l, ev_timer *w, int r) {
    (void)w; (void)r;
    if (++many_fired == 1000) ev_break(l, EVBREAK_ALL);
}
static void test_many_timers(void) {
    many_fired = 0;
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    static ev_timer ts[1000];
    for (int i = 0; i < 1000; ++i) {
        ev_timer_init(&ts[i], many_cb, 0.01 + (i % 10) * 0.001, 0.0);
        ev_timer_start(l, &ts[i]);
    }
    ev_timer wd; ev_timer_init(&wd, wd_cb, 5.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(many_fired == 1000, "1000 concurrent timers all fire (heap scales)");
    ev_loop_destroy(l);
}

#if EV_ASYNC_ENABLE
# ifndef _WIN32
/* ---- ev_async_send from another thread wakes a blocking loop ---- */
static int async_woke;
static void xthread_async_cb(struct ev_loop *l, ev_async *w, int r) { (void)w; (void)r; async_woke = 1; ev_break(l, EVBREAK_ALL); }
static struct ev_loop *xt_loop;
static ev_async        xt_async;
static void *sender(void *arg) {
    (void)arg;
    /* Give the main thread time to block in ev_run. */
    struct timespec ts = {0, 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
    ev_async_send(xt_loop, &xt_async);
    return NULL;
}
static void test_async_from_thread(void) {
    async_woke = 0;
    xt_loop = ev_loop_new(EVFLAG_AUTO);
    ev_async_init(&xt_async, xthread_async_cb);
    ev_async_start(xt_loop, &xt_async);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(xt_loop, &wd);
    pthread_t th;
    if (pthread_create(&th, NULL, sender, NULL) != 0) {
        SKIP("cross-thread ev_async_send", "pthread_create failed");
        ev_loop_destroy(xt_loop);
        return;
    }
    ev_run(xt_loop, 0);
    pthread_join(th, NULL);
    OK(async_woke == 1, "ev_async_send from another thread wakes a blocking loop");
    ev_async_stop(xt_loop, &xt_async);
    ev_loop_destroy(xt_loop);
}
# else
static void test_async_from_thread(void) { SKIP("cross-thread ev_async_send", "POSIX threads only in this test"); }
# endif
#else
static void test_async_from_thread(void) { SKIP("cross-thread ev_async_send", "EV_ASYNC_ENABLE is 0 in this build"); }
#endif

/* ---- the non-blocking pass at its floor (QB-188): a loop that holds no fd and is driven only by
 * EVRUN_NOWAIT passes never polls its backend -- the evpipe behind its signal and async watchers is
 * not counted as a pollable fd, and a pass that cannot sleep raises no wake-up handshake -- and
 * everything the evpipe would have carried still arrives, through the flags, on the next pass.
 * These are the shapes an embedder that drives the loop from its own scheduler hits on every
 * busy pass of a thread that owns a timer, so each is pinned in both directions: delivered, and
 * delivered by the pass right after the send. ---- */
static int nw_async_hits;
#if EV_ASYNC_ENABLE
static void nw_async_cb(struct ev_loop *l, ev_async *w, int r) { (void)l; (void)w; (void)r; ++nw_async_hits; }
static struct ev_loop *nw_loop;
static ev_async        nw_async;
# ifndef _WIN32
/* Cross-thread progress counters (the loop thread and nw_sender): plain `volatile` does not make
   an access atomic to ThreadSanitizer, so the coordination is expressed with the __atomic builtins
   -- RELAXED, since the real ordering is the ev_async wake itself, not these counters. */
#define NW_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define NW_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
static int             nw_hits_seen;   /* the loop thread's count, read by the sender */
static int             nw_late;        /* a send the loop took more than 2 s to deliver: a lost wake, not a slow host */
static int             nw_sent;        /* sender-only until the join */
static void *nw_sender(void *arg) {
    int rounds = *(int *) arg, i;
    for (i = 0; i < rounds; ++i) {
        int       before = NW_LOAD(nw_hits_seen);
        ev_tstamp t0     = ev_time();
        ev_async_send(nw_loop, &nw_async);
        ++nw_sent;
        /* A loop thread that only ever spins NOWAIT passes is a CPU hog, and a loaded host
           (measured: a 20-job build beside this suite) parks it for 100+ ms at a time; the
           budget is for a wake that is LOST -- one the next send would deliver -- not for a
           slow one, so it is wall-clock and generous. */
        while (NW_LOAD(nw_hits_seen) == before && ev_time() - t0 < 2.0) msleep(1);
        if (NW_LOAD(nw_hits_seen) == before) { NW_STORE(nw_late, 1); break; } /* one lost wake is the finding; do not wait 2 s per round for 199 more */
    }
    return NULL;
}
# endif
static void test_nowait_async(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer far_off;
    nw_loop = l;
    ev_async_init(&nw_async, nw_async_cb);
    ev_async_start(l, &nw_async);
    ev_timer_init(&far_off, tick_cb, 3600.0, 0.0); ev_timer_start(l, &far_off); /* what a pending request timeout looks like */
    OK(ev_io_count(l) == 0, "the evpipe an async watcher creates is not a pollable fd: io_count stays 0");
    nw_async_hits = 0;
    ev_run(l, EVRUN_NOWAIT);
    OK(nw_async_hits == 0, "a NOWAIT pass with nothing sent invokes no async callback");
    ev_async_send(l, &nw_async);          /* between two passes: no handshake was raised, so this took the flag path */
    ev_run(l, EVRUN_NOWAIT);
    OK(nw_async_hits == 1, "an ev_async_send between two NOWAIT passes is delivered by the very next pass, with no fd to poll");
    ev_async_send(l, &nw_async); ev_async_send(l, &nw_async);
    ev_run(l, EVRUN_NOWAIT);
    OK(nw_async_hits == 2, "two sends before a pass coalesce into one callback, as they always did");
    {                                     /* a park -- a blocking pass -- and NOWAIT passes again afterwards */
        ev_timer soon; ev_timer_init(&soon, tick_cb, 0.002, 0.0); ev_timer_start(l, &soon);
        tick_fired = 0;
        {                                 /* sleeps until the timer: the evpipe IS polled here. A wait of whole
                                             milliseconds can return a hair before a deadline read on a finer
                                             clock and hand the timer to the next pass (seen once on a hosted
                                             Windows VM, 2026-09-19): the count is the subject, so pass until fired. */
            ev_tstamp t0 = ev_time();
            do ev_run(l, EVRUN_ONCE); while (tick_fired == 0 && ev_time() - t0 < 1.0);
        }
        OK(tick_fired == 1, "a blocking pass over the same loop fires the timer it parked on");
        OK(ev_io_count(l) == 0, "a blocking pass over the same loop still counts no pollable fd afterwards");
        ev_timer_stop(l, &soon);          /* a no-op once it fired; a stack watcher must never outlive its block */
        ev_async_send(l, &nw_async);
        ev_run(l, EVRUN_NOWAIT);
        OK(nw_async_hits == 3, "a send after the park is delivered by the next NOWAIT pass");
    }
# ifndef _WIN32
    {                                     /* another thread sends; this one only ever runs NOWAIT passes: no poll, ever */
        pthread_t th;
        int rounds = 200;
        NW_STORE(nw_hits_seen, nw_async_hits); NW_STORE(nw_late, 0); nw_sent = 0;
        if (pthread_create(&th, NULL, nw_sender, &rounds) == 0) {
            ev_tstamp t0 = ev_time();
            while (!NW_LOAD(nw_late) && nw_async_hits < 3 + rounds) { /* until the last send is delivered, not merely sent */
                ev_run(l, EVRUN_NOWAIT);
                NW_STORE(nw_hits_seen, nw_async_hits);
                if (ev_time() - t0 > 20.0) break;   /* the watchdog: a lost wake would otherwise spin forever */
            }
            pthread_join(th, NULL);
            if (nw_late || nw_async_hits != 3 + rounds)
                printf("        (late=%d sent=%d hits=%d expected=%d)\n", nw_late, nw_sent, nw_async_hits, 3 + rounds);
            OK(nw_late == 0 && nw_async_hits == 3 + rounds,
               "200 cross-thread sends, each delivered by a NOWAIT-only loop before the next, none coalesced, none lost");
        } else
            SKIP("cross-thread sends into a NOWAIT-only loop", "pthread_create failed");
    }
    {                                     /* the mixed shape: parks (blocking passes) interleaved with NOWAIT passes while
                                             the other thread keeps sending -- the window in which a sender WRITES the
                                             evpipe (it saw the handshake up) and the byte lands after the poll returned */
        pthread_t th;
        int rounds = 200;
        ev_timer cap; ev_timer_init(&cap, tick_cb, 0.0005, 0.0005); ev_timer_start(l, &cap);
        NW_STORE(nw_hits_seen, nw_async_hits); NW_STORE(nw_late, 0); nw_sent = 0;
        if (pthread_create(&th, NULL, nw_sender, &rounds) == 0) {
            ev_tstamp t0 = ev_time();
            while (!NW_LOAD(nw_late) && nw_async_hits < 3 + 2 * rounds) {
                int i;
                ev_run(l, EVRUN_ONCE);    /* a park of at most 0.5 ms */
                NW_STORE(nw_hits_seen, nw_async_hits);
                for (i = 0; i < 50; ++i) { ev_run(l, EVRUN_NOWAIT); NW_STORE(nw_hits_seen, nw_async_hits); }
                if (ev_time() - t0 > 20.0) break;
            }
            pthread_join(th, NULL);
            if (nw_late || nw_async_hits != 3 + 2 * rounds)
                printf("        (late=%d sent=%d hits=%d expected=%d)\n", nw_late, nw_sent, nw_async_hits, 3 + 2 * rounds);
            OK(nw_late == 0 && nw_async_hits == 3 + 2 * rounds,
               "200 sends into a loop alternating parks and NOWAIT passes: each delivered before the next, none lost");
        } else
            SKIP("cross-thread sends across parks and NOWAIT passes", "pthread_create failed");
        ev_timer_stop(l, &cap);
    }
# endif
    ev_timer_stop(l, &far_off);
    ev_async_stop(l, &nw_async);
    ev_loop_destroy(l);
}
#else
static void test_nowait_async(void) { SKIP("async delivery into a NOWAIT-only loop", "EV_ASYNC_ENABLE is 0 in this build"); }
#endif

#if EV_SIGNAL_ENABLE && !defined(_WIN32)
static int nw_sig_hits;
static void nw_sig_cb(struct ev_loop *l, ev_signal *w, int r) { (void)l; (void)w; (void)r; ++nw_sig_hits; }
static void test_nowait_signal(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);   /* the default loop's sighandler + evpipe path, not signalfd */
    ev_signal sw;
    ev_timer far_off;
    ev_signal_init(&sw, nw_sig_cb, SIGUSR1); ev_signal_start(l, &sw);
    ev_timer_init(&far_off, tick_cb, 3600.0, 0.0); ev_timer_start(l, &far_off);
    OK(ev_io_count(l) == 0, "the evpipe a signal watcher creates is not a pollable fd either");
    nw_sig_hits = 0;
    ev_run(l, EVRUN_NOWAIT);
    raise(SIGUSR1);                       /* the handler runs here, between two passes, and takes the flag path */
    ev_run(l, EVRUN_NOWAIT);
    OK(nw_sig_hits == 1, "a signal raised between two NOWAIT passes is delivered by the very next pass, with no fd to poll");
    ev_timer_stop(l, &far_off);
    ev_signal_stop(l, &sw);
    ev_loop_destroy(l);
}
#else
static void test_nowait_signal(void) { SKIP("signal delivery into a NOWAIT-only loop", "POSIX signals with EV_SIGNAL_ENABLE only"); }
#endif

static void test_nowait_clock(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer far_off;
    ev_tstamp before;
    ev_timer_init(&far_off, tick_cb, 3600.0, 0.0); ev_timer_start(l, &far_off);
    ev_run(l, EVRUN_NOWAIT);
    before = ev_now(l);
    msleep(5);
    ev_run(l, EVRUN_NOWAIT);              /* one clock read per NOWAIT pass, not two -- and not zero */
    OK(ev_now(l) - before >= 0.004, "a NOWAIT pass still advances ev_now (its one clock read is the post-poll one)");
    {                                     /* the read is what an expiring timer is judged against */
        ev_timer soon; ev_timer_init(&soon, tick_cb, 0.003, 0.0); ev_timer_start(l, &soon);
        tick_fired = 0;
        ev_run(l, EVRUN_NOWAIT);
        OK(tick_fired == 0, "a timer 3 ms out does not fire on a pass that runs at once");
        msleep(5);
        ev_run(l, EVRUN_NOWAIT);
        OK(tick_fired == 1, "and fires on the first pass after its deadline, judged on that pass's own clock read");
        ev_timer_stop(l, &soon);
    }
    ev_timer_stop(l, &far_off);
    ev_loop_destroy(l);
}

/* ---- EVRUN_NOPOLL: the embedder owns the io cadence (QB-191) ---- */
/* A non-blocking pass with the flag reifies timers and invokes pending events but never calls
 * the backend poll; the next pass without it delivers what the fd holds; a BLOCKING pass ignores
 * the flag, because with an fd to wait on the poll is the wake. */
static int nopoll_io_hits;
static void nopoll_io_cb(struct ev_loop *l, ev_io *w, int r) { char c; (void)l; (void)r; (void) !read(w->fd, &c, 1); ++nopoll_io_hits; } /* drains, so the fd is quiet again */
static void test_nopoll(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t;
    OK(*ev_io_count_addr(l) == (int) ev_io_count(l) && ev_io_count(l) == 0, "ev_io_count_addr reads the count ev_io_count reports");
    ev_timer_init(&t, tick_cb, 0.0, 0.0); ev_timer_start(l, &t);
    msleep(2);
    tick_fired = 0;
    ev_run(l, EVRUN_NOWAIT | EVRUN_NOPOLL);
    OK(tick_fired == 1, "a NOWAIT|NOPOLL pass still fires an expired timer");
    ev_timer_init(&t, fed_cb, 3600.0, 0.0); ev_timer_start(l, &t);
    ev_feed_event(l, &t, EV_CUSTOM);
    fed = 0;
    ev_run(l, EVRUN_NOWAIT | EVRUN_NOPOLL);
    OK(fed == 1, "a NOWAIT|NOPOLL pass still invokes a pending (fed) event");
    ev_timer_stop(l, &t);
#ifndef _WIN32
    {
        int   fds[2];
        ev_io w;
        if (pipe(fds) == 0) {
            ev_io_init(&w, nopoll_io_cb, fds[0], EV_READ); ev_io_start(l, &w);
            OK(*ev_io_count_addr(l) == 1, "the inline count follows ev_io_start");
            nopoll_io_hits = 0;
            (void) !write(fds[1], "x", 1);
            {
                const unsigned int fed0 = *ev_io_fed_addr(l);
                ev_run(l, EVRUN_NOWAIT | EVRUN_NOPOLL);
                OK(nopoll_io_hits == 0 && *ev_io_fed_addr(l) == fed0, "a readable fd is NOT delivered by a NOWAIT|NOPOLL pass: no backend poll ran, the fed count did not move");
                ev_run(l, EVRUN_NOWAIT);
                OK(nopoll_io_hits == 1 && *ev_io_fed_addr(l) == fed0 + 1, "the next NOWAIT pass without the flag delivers it, and the fed count says the poll found one fd");
                ev_run(l, EVRUN_NOWAIT);
                OK(*ev_io_fed_addr(l) == fed0 + 1, "a poll that finds nothing leaves the fed count alone");
            }
            (void) !write(fds[1], "y", 1);
            ev_run(l, EVRUN_ONCE | EVRUN_NOPOLL);
            OK(nopoll_io_hits == 2, "a blocking pass ignores the flag: with an fd to wait on, the poll is the wake");
            ev_io_stop(l, &w);
            close(fds[0]); close(fds[1]);
        } else {
            SKIP("EVRUN_NOPOLL over a readable fd", "pipe() failed");
        }
    }
#endif
    ev_loop_destroy(l);
}

/* ---- the embedder supplies the pass's clock (QB-190) ---- */
/* qb's core reads the monotonic clock on its idle passes anyway; ev_now_set hands that reading
   to the loop so the NOWAIT pass that follows reads none of its own. Timers are judged against
   the supplied time -- a sample ahead of the wall fires a far timer at once -- the loop's clock
   never steps back for an old sample, the supply stands for ONE pass, and a blocking pass
   always re-reads. */
static int ns_fired;
static void ns_cb(struct ev_loop *l, ev_timer *w, int r) { (void)l; (void)w; (void)r; ++ns_fired; }
static void test_now_set(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer t;
    ev_run(l, EVRUN_NOWAIT);                                    /* one plain pass: the loop's own reading */
    {
        ev_tstamp c0 = ev_clock_now(), c1;
        msleep(2);
        c1 = ev_clock_now();
        OK(c1 > c0 && c1 - c0 < 1.0, "ev_clock_now reads the loop's monotonic clock: it advances across a sleep");
    }
    ev_timer_init(&t, ns_cb, 5.0, 0.0);                          /* a timer five seconds out */
    ev_now_set(l, ev_clock_now());
    ev_timer_start(l, &t);
    ns_fired = 0;
    ev_run(l, EVRUN_NOWAIT);
    OK(ns_fired == 0, "a five-second timer armed against a fresh supplied clock does not fire on that pass");
    ev_now_set(l, ev_clock_now() + 10.0);                        /* the embedder says it is ten seconds later */
    ev_run(l, EVRUN_NOWAIT);
    OK(ns_fired == 1, "the loop judges its timers against the SUPPLIED time: the pass fires the timer at once, no clock read of its own");
    {
        ev_timer z; ev_tstamp ahead;
        ev_timer_init(&z, ns_cb, 0.0, 0.0); ev_timer_start(l, &z); /* a zero timer's deadline IS the loop's time */
        ahead = ev_timer_next(l);
        ev_timer_stop(l, &z);
        ev_now_set(l, ev_clock_now() - 100.0);                   /* an older sample ... */
        ev_timer_start(l, &z);
        OK(ahead > ev_clock_now() + 9.0 && ev_timer_next(l) >= ahead, "a supplied sample older than the loop's time is ignored: the loop's clock never steps back");
        ev_timer_stop(l, &z);
    }
    ev_loop_destroy(l);
    l = ev_loop_new(EVFLAG_AUTO);
    ev_run(l, EVRUN_NOWAIT);
    {
        ev_tstamp rt0;
        ev_now_set(l, ev_clock_now());
        ev_run(l, EVRUN_NOWAIT);                                 /* consumes the supply */
        rt0 = ev_now(l);
        msleep(20);
        ev_run(l, EVRUN_NOWAIT);                                 /* no supply: this pass reads its own clock */
        OK(ev_now(l) - rt0 >= 0.015, "a supplied clock stands for one NOWAIT pass: the next one reads the clock again (20 ms slept, the loop saw them)");
        ev_now_set(l, ev_clock_now());                           /* a supply, then a BLOCKING pass */
        ev_timer_init(&t, ns_cb, 0.01, 0.0); ev_timer_start(l, &t);
        {
            ev_tstamp t0 = ev_time(), rt1;
            ev_run(l, EVRUN_ONCE);
            OK(ev_time() - t0 >= 0.005 && ev_time() - t0 < 0.5, "a blocking pass after a supply still sleeps until its timer: a wait reads the clock it needs");
            rt1 = ev_now(l);
            msleep(20);
            ev_run(l, EVRUN_NOWAIT);
            OK(ev_now(l) - rt1 >= 0.015, "and the supply did not outlive it: the NOWAIT pass after the wait read the clock again");
        }
    }
    ev_loop_destroy(l);
}

/* ---- the earliest deadline and the timer count, read without the loop (QB-190) ---- */
static void test_timer_next(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_timer a, b;
    ev_run(l, EVRUN_NOWAIT);
    OK(*ev_timer_count_addr(l) == 0 && ev_timer_next(l) >= 1e12, "no timer: the count reads 0 and the earliest deadline is beyond any reading (1e12 s or more)");
    ev_now_update(l);
    ev_timer_init(&a, ns_cb, 3.0, 0.0); ev_timer_start(l, &a);
    ev_timer_init(&b, ns_cb, 1.0, 0.0); ev_timer_start(l, &b);
    {
        ev_tstamp next = ev_timer_next(l), now = ev_clock_now();
        OK(*ev_timer_count_addr(l) == 2, "the count follows ev_timer_start");
        OK(next > now + 0.9 && next < now + 1.1, "the earliest deadline is the one-second timer's, on ev_clock_now's scale");
        OK(next > now, "an embedder comparing its own reading sees the deadline ahead: nothing to fire this pass");
    }
    ev_timer_stop(l, &b);
    OK(*ev_timer_count_addr(l) == 1 && ev_timer_next(l) > ev_clock_now() + 2.5, "stopping the earliest timer moves the deadline to the next one");
    ev_now_set(l, ev_timer_next(l) + 0.001);                     /* the embedder's reading passes the deadline */
    ns_fired = 0;
    ev_run(l, EVRUN_NOWAIT);
    OK(ns_fired == 1 && *ev_timer_count_addr(l) == 0 && ev_timer_next(l) >= 1e12, "a reading at or past the deadline means due: the pass fires it, and the count and the deadline read empty again");
    ev_loop_destroy(l);
}

#if EV_ASYNC_ENABLE
/* ---- a wake that no pass has delivered, read without the loop (QB-190) ---- */
static int wp_hits;
static void wp_cb(struct ev_loop *l, ev_async *w, int r) { (void)l; (void)w; (void)r; ++wp_hits; }
static void test_wake_pending(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_async a;
    ev_async_init(&a, wp_cb); ev_async_start(l, &a);
    ev_run(l, EVRUN_NOWAIT);
    OK(*ev_wake_pending_addr(l) == 0, "nothing sent: no wake pending");
    ev_async_send(l, &a);                                        /* between passes: the flag path */
    OK(*ev_wake_pending_addr(l) != 0, "an ev_async_send between two passes is a pending wake an embedder can read inline");
    wp_hits = 0;
    ev_run(l, EVRUN_NOWAIT);
    OK(wp_hits == 1 && *ev_wake_pending_addr(l) == 0, "the pass that delivers it clears the flag");
    ev_async_stop(l, &a);
    ev_loop_destroy(l);
}
#endif

#ifndef _WIN32
/* ---- concurrent ev_loop_new from N threads (QB-192) ----
   Every VirtualCore thread in qb creates its own loop with ev_loop_new, which runs loop_init's
   have_realtime / have_monotonic capability probe -- file-scope flags shared by ALL loops. N
   threads doing that at once read and write those flags concurrently; before QB-192 that was a
   data race (idempotent, but a race), invisible until ev.c was instrumented. This drives the
   exact shape: THREADS loops created, each run once and destroyed, all at the same time. It
   asserts every loop came up with a working clock; under ThreadSanitizer it asserts the absence
   of the loop_init race (a plain build cannot see it, which is the whole point of the gate). */
#define NLOOP_THREADS 8
static int       lc_ok[NLOOP_THREADS];
static void lc_timer_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ev_break(l, EVBREAK_ALL); }
static void *lc_thread(void *arg) {
    int idx = *(int *) arg;
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);   /* <- loop_init: the capability probe */
    if (l) {
        ev_timer t;
        ev_timer_init(&t, lc_timer_cb, 0.001, 0.0); ev_timer_start(l, &t);
        ev_run(l, 0);                                /* one real turn: the clock must advance a timer to fire it */
        lc_ok[idx] = (ev_now(l) > 0.);               /* the loop got a working clock from loop_init */
        ev_timer_stop(l, &t);
        ev_loop_destroy(l);
    }
    return NULL;
}
static void test_concurrent_loop_new(void) {
    pthread_t th[NLOOP_THREADS];
    int       id[NLOOP_THREADS], i, started = 0, all_ok = 1;
    for (i = 0; i < NLOOP_THREADS; ++i) { lc_ok[i] = 0; id[i] = i; }
    for (i = 0; i < NLOOP_THREADS; ++i)
        if (pthread_create(&th[i], NULL, lc_thread, &id[i]) == 0) ++started;
    for (i = 0; i < started; ++i) pthread_join(th[i], NULL);
    if (started < NLOOP_THREADS) { SKIP("concurrent ev_loop_new", "pthread_create failed"); return; }
    for (i = 0; i < NLOOP_THREADS; ++i) all_ok = all_ok && lc_ok[i];
    OK(all_ok, "N threads each create a loop, run a timer to completion and destroy it, concurrently: every loop's clock works (loop_init's capability probe is race-free -- QB-192)");
}

/* ---- two threads sending into ONE loop at once (QB-192) ----
   The wake protocol's async_pending flag and each ev_async.sent are written by ANY sending
   thread and read/cleared by the loop thread. One sender is test_nowait_async; two senders,
   each to its own async watcher, exercise the flags under real concurrent writers -- the shape
   TSan must find clean once ev.c is instrumented, and a lost/misattributed wake would drop a
   count here. Each sender does ROUNDS sends and waits for its own watcher to catch up. */
#define TS_ROUNDS 300
static ev_async ts_a[2];
static int      ts_hits[2];
static int      ts_seen[2];       /* the loop thread's published per-watcher count */
static void ts_cb0(struct ev_loop *l, ev_async *w, int r) { (void)l; (void)w; (void)r; ++ts_hits[0]; }
static void ts_cb1(struct ev_loop *l, ev_async *w, int r) { (void)l; (void)w; (void)r; ++ts_hits[1]; }
static void *ts_sender(void *arg) {
    int k = *(int *) arg, i;
    for (i = 0; i < TS_ROUNDS; ++i) {
        int before = __atomic_load_n(&ts_seen[k], __ATOMIC_RELAXED);
        ev_async_send(nw_loop, &ts_a[k]);
        ev_tstamp t0 = ev_time();
        while (__atomic_load_n(&ts_seen[k], __ATOMIC_RELAXED) == before && ev_time() - t0 < 2.0) msleep(1);
        if (__atomic_load_n(&ts_seen[k], __ATOMIC_RELAXED) == before) break; /* a lost wake */
    }
    return NULL;
}
static void test_async_two_senders(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    pthread_t th[2];
    int       id[2] = {0, 1}, i, started = 0;
    ev_timer  far_off;
    nw_loop = l;
    ts_hits[0] = ts_hits[1] = ts_seen[0] = ts_seen[1] = 0;
    ev_async_init(&ts_a[0], ts_cb0); ev_async_start(l, &ts_a[0]);
    ev_async_init(&ts_a[1], ts_cb1); ev_async_start(l, &ts_a[1]);
    ev_timer_init(&far_off, tick_cb, 3600.0, 0.0); ev_timer_start(l, &far_off);
    for (i = 0; i < 2; ++i) if (pthread_create(&th[i], NULL, ts_sender, &id[i]) == 0) ++started;
    if (started == 2) {
        ev_tstamp t0 = ev_time();
        while ((ts_hits[0] < TS_ROUNDS || ts_hits[1] < TS_ROUNDS) && ev_time() - t0 < 20.0) {
            ev_run(l, EVRUN_NOWAIT);
            __atomic_store_n(&ts_seen[0], ts_hits[0], __ATOMIC_RELAXED);
            __atomic_store_n(&ts_seen[1], ts_hits[1], __ATOMIC_RELAXED);
        }
        for (i = 0; i < started; ++i) pthread_join(th[i], NULL);
        OK(ts_hits[0] == TS_ROUNDS && ts_hits[1] == TS_ROUNDS,
           "two threads sending into one loop at once: every send on each of the two async watchers is delivered, none lost, none misattributed (QB-192)");
    } else {
        for (i = 0; i < started; ++i) pthread_join(th[i], NULL);
        SKIP("two concurrent async senders", "pthread_create failed");
    }
    ev_async_stop(l, &ts_a[0]); ev_async_stop(l, &ts_a[1]);
    ev_timer_stop(l, &far_off);
    ev_loop_destroy(l);
}
#endif

#ifdef __linux__
/* ---- io_uring: non-blocking passes over a quiet fd make no syscall storm (QB-81) ----
   The backend armed its timerfd at "now" on every timeout-0 poll: it expired at once, its one-shot
   POLL_ADD completed, the drain reset the deadline, the POLL_ADD was re-armed through io_uring_enter,
   and the next pass armed "now" again -- three syscalls per cycle, the quiet-socket pass measured
   at 47x epoll's whatever qb's poll cadence. The mechanism is not reachable from the API, so the
   witness is the ratio a user sees: N NOWAIT passes over one quiet pipe end on an io_uring loop
   against the same on an epoll loop, back to back in this process (host speed and a sanitizer's
   slow-down cancel out: the storm measured ~10x under TSan, the fix ~0.3x). Each side is the better
   of two alternated runs, so one descheduling cannot fail it. The fd never fires: its callback
   counts rather than aborts, and that count is asserted. */
#define QP_PASSES 200000
static int qp_fired;
static void qp_quiet_cb(struct ev_loop *l, ev_io *w, int r) { (void)l; (void)w; (void)r; ++qp_fired; }
static double qp_nowait_seconds(unsigned int backend, int *ok) {
    struct ev_loop *l = ev_loop_new(backend);
    int fds[2], i; ev_io w; ev_tstamp t0, t1;
    *ok = 0;
    if (!l) return 0.;
    if (pipe(fds) != 0) { ev_loop_destroy(l); return 0.; }
    ev_io_init(&w, qp_quiet_cb, fds[0], EV_READ); ev_io_start(l, &w);
    ev_run(l, EVRUN_NOWAIT);                        /* the one real registration with the backend */
    t0 = ev_time();
    for (i = 0; i < QP_PASSES; ++i) ev_run(l, EVRUN_NOWAIT);
    t1 = ev_time();
    ev_io_stop(l, &w); close(fds[0]); close(fds[1]); ev_loop_destroy(l);
    *ok = 1;
    return t1 - t0;
}
static void test_iouring_quiet_nowait(void) {
    const char *name = "io_uring: N non-blocking passes over a quiet fd cost no more than 2x epoll's (the timerfd re-armed at now on every pass made it 47x -- QB-81)";
    double u1, e1, u2, e2, u, e; int oku = 0, oke = 0;
    if (!(ev_supported_backends() & EVBACKEND_IOURING)) { SKIP(name, "io_uring not compiled in"); return; }
    if (!(ev_supported_backends() & EVBACKEND_EPOLL))   { SKIP(name, "no epoll to compare against"); return; }
    qp_fired = 0;
    u1 = qp_nowait_seconds(EVBACKEND_IOURING, &oku);
    if (!oku) { SKIP(name, "io_uring unavailable at runtime (ev_loop_new returned NULL)"); return; }
    e1 = qp_nowait_seconds(EVBACKEND_EPOLL, &oke);
    u2 = qp_nowait_seconds(EVBACKEND_IOURING, &oku);
    e2 = qp_nowait_seconds(EVBACKEND_EPOLL, &oke);
    if (!oke || !oku) { SKIP(name, "a loop or pipe could not be created"); return; }
    u = u1 < u2 ? u1 : u2; e = e1 < e2 ? e1 : e2;
    printf("        io_uring %.1f ms, epoll %.1f ms for %d quiet NOWAIT passes each (ratio %.2f), fd fired %d times\n",
           u * 1e3, e * 1e3, QP_PASSES, e > 0. ? u / e : 0., qp_fired);
    OK(qp_fired == 0 && e > 0. && u <= 2.0 * e, name);
}

/* ---- io_uring: a BLOCKING wait still honours its timer deadline (the positive control of QB-81) ----
   The fix arms the timerfd only when the poll will sleep; this proves the sleeping path still does:
   an io_uring loop holding one 20 ms timer, run blocking, must return in about 20 ms -- neither at
   once (a deadline never armed would wake on nothing... or hang) nor much later. */
static void test_iouring_blocking_timer(void) {
    const char *name = "io_uring: a blocking run over a 20 ms timer returns in 15..500 ms (the timerfd is still armed for a real sleep -- QB-81)";
    struct ev_loop *l; ev_timer t; ev_tstamp t0, dt;
    if (!(ev_supported_backends() & EVBACKEND_IOURING)) { SKIP(name, "io_uring not compiled in"); return; }
    l = ev_loop_new(EVBACKEND_IOURING);
    if (!l) { SKIP(name, "io_uring unavailable at runtime"); return; }
    tick_fired = 0;
    ev_timer_init(&t, tick_cb, 0.020, 0.0); ev_timer_start(l, &t);
    t0 = ev_time();
    ev_run(l, 0);
    dt = ev_time() - t0;
    ev_timer_stop(l, &t);
    printf("        blocking run over a 20 ms timer under io_uring returned after %.1f ms\n", dt * 1e3);
    OK(tick_fired == 1 && dt >= 0.015 && dt <= 0.5, name);
    ev_loop_destroy(l);
}

/* ---- epoll: a blocking wait under a millisecond is honoured under a millisecond (QB-196) ---- */
/* epoll_wait takes whole milliseconds and libev rounds UP, so a blocking run over a 200 us timer
 * slept a full millisecond on this backend: measured in qb's core, a 100 us timer on a parked
 * thread fired 1010 us late. Through epoll_pwait2 the kernel honours a timespec to the thread's
 * timer slack (50 us by default). Twenty rounds and the MINIMUM kept: the millisecond path cannot
 * return before 1 ms, so one round under 800 us is the nanosecond path and nothing else, and a
 * loaded host only ever moves a round upward. The kernel is asked first, the same question the
 * loop asks at init: one without epoll_pwait2 (< 5.11) makes the case a SKIP, never a FAIL. */
static void test_epoll_ns_wait(void) {
    const char *name = "epoll: a blocking run over a 200 us timer returns under 800 us at least once in 20 (epoll_pwait2 -- QB-196)";
#if EV_USE_EPOLL_PWAIT2
    struct ev_loop *l; ev_timer t; ev_tstamp best = 1.0; int i;
    if (!(ev_supported_backends() & EVBACKEND_EPOLL)) { SKIP(name, "epoll not compiled in"); return; }
    {
        struct timespec zero = {0, 0}; struct epoll_event e[1];
        int fd = epoll_create1(0), r = fd >= 0 ? epoll_pwait2(fd, e, 1, &zero, 0) : -1;
        int enosys = r < 0 && errno == ENOSYS;
        if (fd >= 0) close(fd);
        if (enosys) { SKIP(name, "the kernel has no epoll_pwait2 (Linux < 5.11)"); return; }
    }
    l = ev_loop_new(EVBACKEND_EPOLL | EVFLAG_NOENV);
    if (!l || ev_backend(l) != EVBACKEND_EPOLL) { SKIP(name, "epoll unavailable at runtime"); if (l) ev_loop_destroy(l); return; }
    for (i = 0; i < 20; ++i) {
        ev_tstamp t0, dt;
        tick_fired = 0;
        ev_timer_init(&t, tick_cb, 0.0002, 0.0); ev_timer_start(l, &t);
        t0 = ev_time();
        ev_run(l, 0);
        dt = ev_time() - t0;
        ev_timer_stop(l, &t);
        if (tick_fired == 1 && dt < best) best = dt;
    }
    printf("        blocking run over a 200 us timer under epoll: the best of 20 returned after %.0f us\n", best * 1e6);
    OK(best < 0.0008, name);
    ev_loop_destroy(l);
#else
    SKIP(name, "epoll_pwait2 not declared by this libc (glibc < 2.35)");
#endif
}
#endif

/* ---- the loop's clock: sub-millisecond, and it never steps back (QB-193) ---- */
/* On Windows libev read both its clocks from GetSystemTimeAsFileTime, the system tick -- 1 to
 * 15.6 ms steps -- and judged every timer against it; qev reads QueryPerformanceCounter. A
 * timer started and run inside one tick is the shape test_io_count begins with, and it is why
 * that case failed on MSVC before this one existed. */
static void test_clock_resolution(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_tstamp t0 = ev_time(), first, last;
    int distinct = 0, backwards = 0;
    ev_now_update(l);
    first = last = ev_now(l);
    while (ev_time() - t0 < 0.05) {       /* 50 ms of back-to-back reads */
        ev_tstamp now;
        ev_now_update(l);
        now = ev_now(l);
        if (now != last) ++distinct;
        if (now < last) ++backwards;
        last = now;
    }
    OK(distinct >= 1000, "ev_now moves at least 1000 times in 50 ms of back-to-back reads: a clock finer than 50 us, not the system tick");
    OK(backwards == 0, "ev_now never steps back across back-to-back reads");
    OK(last - first >= 0.045 && last - first <= 0.5, "and the loop's clock measures the 50 ms the wall clock did");
    ev_loop_destroy(l);
}

#ifndef _WIN32
/* ---- a real fork(): the child re-arms with ev_loop_fork and its loop works ---- */
static int postfork_hit;
static void postfork_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; postfork_hit = 1; ev_break(l, EVBREAK_ALL); }
static void test_real_fork(void) {
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    /* Prove the loop worked pre-fork so the child result is meaningful. */
    ev_timer warm; ev_timer_init(&warm, stop_cb, 0.01, 0.0); ev_timer_start(l, &warm);
    ev_run(l, 0);
    pid_t pid = fork();
    if (pid < 0) { SKIP("real fork + ev_loop_fork", "fork failed"); ev_loop_destroy(l); return; }
    if (pid == 0) {
        ev_loop_fork(l); /* re-arm backend fds in the child */
        postfork_hit = 0;
        ev_timer t; ev_timer_init(&t, postfork_cb, 0.02, 0.0); ev_timer_start(l, &t);
        ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
        ev_run(l, 0);
        _exit(postfork_hit == 1 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    OK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
       "after fork + ev_loop_fork, the child's loop still fires timers");
    ev_loop_destroy(l);
}
#else
static void test_real_fork(void) { SKIP("real fork + ev_loop_fork", "POSIX-only in this test"); }
#endif

int main(void) {
    printf("qev loop-mechanics coverage (ev %d.%d, backends=0x%x)\n",
           ev_version_major(), ev_version_minor(), ev_supported_backends());

    test_multi_loop_isolation();
    test_timer_ordering();
    test_timer_repeat();
    test_timer_again();
    test_priorities();
    test_backend_unavailable();
    test_now_update();
    test_iteration_count();
    test_active_count();
    test_count_addr();
    test_io_count();
    test_feed_event();
    test_stop_inactive();
    test_many_timers();
    test_async_from_thread();
    test_nowait_async();
    test_nowait_signal();
    test_nowait_clock();
    test_clock_resolution();
    test_nopoll();
    test_now_set();
    test_timer_next();
#if EV_ASYNC_ENABLE
    test_wake_pending();
#else
    SKIP("wake pending (ev_wake_pending_addr)", "async watchers compiled out");
#endif
#ifndef _WIN32
    test_concurrent_loop_new();
# if EV_ASYNC_ENABLE
    test_async_two_senders();
# else
    SKIP("two concurrent async senders", "async watchers compiled out");
# endif
#endif
#ifdef __linux__
    test_iouring_quiet_nowait();
    test_iouring_blocking_timer();
    test_epoll_ns_wait();
#endif
    test_real_fork();

    printf("\n== loops: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);

    /* Forty-eight checks are unconditional in every profile on every platform (only the
       cross-thread async cases, the fork case, the signal half of the NOWAIT cases, the pipe
       halves of the io-count and NOPOLL cases, the wake-pending case of a profile without
       async watchers and the nanosecond epoll wait on a libc or kernel without epoll_pwait2
       can legitimately skip, and the backend probe skips only where every backend exists,
       which no platform has). A run below that floor measured nothing and must not read as a
       pass. */
    if (g_run < 48) {
        printf("== FAIL: only %d checks ran; at least 48 are unconditional ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}
