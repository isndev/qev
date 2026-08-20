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

#ifndef _WIN32
# include <pthread.h>
# include <sys/types.h>
# include <sys/wait.h>
# include <unistd.h>
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
    test_feed_event();
    test_stop_inactive();
    test_many_timers();
    test_async_from_thread();
    test_real_fork();

    printf("\n== loops: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);

    /* Ten checks are unconditional in every profile on every platform (only the
       cross-thread async and the fork case can legitimately skip, and the backend
       probe skips only where every backend exists, which no platform has). A run
       below that floor measured nothing and must not read as a pass. */
    if (g_run < 10) {
        printf("== FAIL: only %d checks ran; at least 10 are unconditional ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}
