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
        ev_run(l, EVRUN_ONCE);
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
static volatile int    nw_hits_seen;   /* the loop thread's count, read by the sender */
static volatile int    nw_late;        /* a send the loop took more than 2 s to deliver: a lost wake, not a slow host */
static int             nw_sent;
static void *nw_sender(void *arg) {
    int rounds = *(int *) arg, i;
    for (i = 0; i < rounds; ++i) {
        int       before = nw_hits_seen;
        ev_tstamp t0     = ev_time();
        ev_async_send(nw_loop, &nw_async);
        ++nw_sent;
        /* A loop thread that only ever spins NOWAIT passes is a CPU hog, and a loaded host
           (measured: a 20-job build beside this suite) parks it for 100+ ms at a time; the
           budget is for a wake that is LOST -- one the next send would deliver -- not for a
           slow one, so it is wall-clock and generous. */
        while (nw_hits_seen == before && ev_time() - t0 < 2.0) msleep(1);
        if (nw_hits_seen == before) { nw_late = 1; break; } /* one lost wake is the finding; do not wait 2 s per round for 199 more */
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
        ev_run(l, EVRUN_ONCE);            /* sleeps until the timer: the evpipe IS polled here */
        OK(tick_fired == 1 && ev_io_count(l) == 0, "a blocking pass over the same loop still counts no pollable fd afterwards");
        ev_timer_stop(l, &soon);          /* a no-op once it fired; a stack watcher must never outlive its block */
        ev_async_send(l, &nw_async);
        ev_run(l, EVRUN_NOWAIT);
        OK(nw_async_hits == 3, "a send after the park is delivered by the next NOWAIT pass");
    }
# ifndef _WIN32
    {                                     /* another thread sends; this one only ever runs NOWAIT passes: no poll, ever */
        pthread_t th;
        int rounds = 200;
        nw_hits_seen = nw_async_hits; nw_late = 0; nw_sent = 0;
        if (pthread_create(&th, NULL, nw_sender, &rounds) == 0) {
            ev_tstamp t0 = ev_time();
            while (!nw_late && nw_async_hits < 3 + rounds) { /* until the last send is delivered, not merely sent */
                ev_run(l, EVRUN_NOWAIT);
                nw_hits_seen = nw_async_hits;
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
        nw_hits_seen = nw_async_hits; nw_late = 0; nw_sent = 0;
        if (pthread_create(&th, NULL, nw_sender, &rounds) == 0) {
            ev_tstamp t0 = ev_time();
            while (!nw_late && nw_async_hits < 3 + 2 * rounds) {
                int i;
                ev_run(l, EVRUN_ONCE);    /* a park of at most 0.5 ms */
                nw_hits_seen = nw_async_hits;
                for (i = 0; i < 50; ++i) { ev_run(l, EVRUN_NOWAIT); nw_hits_seen = nw_async_hits; }
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
    test_real_fork();

    printf("\n== loops: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);

    /* Thirty-two checks are unconditional in every profile on every platform (only the
       cross-thread async cases, the fork case, the signal half of the NOWAIT cases and the
       pipe half of the io-count case can legitimately skip, and the backend probe skips only
       where every backend exists, which no platform has). A run below that floor measured
       nothing and must not read as a pass. */
    if (g_run < 32) {
        printf("== FAIL: only %d checks ran; at least 32 are unconditional ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}
