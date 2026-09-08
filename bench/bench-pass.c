/*
 * qev -- what one non-blocking pass of the loop costs, by shape (standalone, no external deps).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The instrument behind qev's pass-cost work (Huly QB-186 and the qev performance programme of
 * qb 3.2.0). An embedder that drives the loop from its own scheduler -- qb's VirtualCore -- calls
 * ev_run (EVRUN_NOWAIT) once per pass of its own, so what that call costs when there is NOTHING
 * to deliver is paid on every pass of every core that owns a timer or a socket. Each shape below
 * runs N passes over a loop holding a fixed set of watchers that never fire, and prints the
 * nanoseconds per pass; the arm/disarm shapes measure one ev_timer_start + ev_timer_stop, and one
 * ev_now_update, which qb's request timeouts pay per request.
 *
 *   empty         a fresh loop, nothing registered (ev_run's own bookkeeping)
 *   timer         one ev_timer far in the future (the shape a request timeout leaves behind)
 *   fd            one socketpair end, never readable (a quiet socket: the backend poll is real)
 *   fd+timer      both
 *   arm           ev_timer_start + ev_timer_stop of a 500 ms timer, mn_now as it is
 *   arm+now       the same preceded by ev_now_update, as qb's awaiters do
 *   now           ev_now_update alone
 *   timer+set     the timer shape with the embedder supplying the clock: ev_clock_now () +
 *                 ev_now_set () before each pass, so the pass reads none of its own (QB-190)
 *   timer+set0    the same with a free sample (a counter the embedder already has): the loop's
 *                 bookkeeping floor for a pass given its time
 *   gate          no ev_run at all: what an embedder pays per pass to decide the loop has
 *                 nothing to fire -- *ev_timer_count_addr, ev_timer_next against its reading,
 *                 *ev_wake_pending_addr (the pass qb skips when a far timer is all it holds)
 *
 *   qev-bench-pass [passes=2000000] [shape ...]      (no shape = all)
 */
#include <qev/ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
# include <sys/socket.h>
# include <unistd.h>
#endif

#ifdef QEV_BENCH_NO_IO_COUNT /* a control archive older than ev_io_count () */
static unsigned ev_io_count(struct ev_loop *l) { (void)l; return 0u; }
#endif

static double now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

static void never_cb(struct ev_loop *l, ev_timer *w, int r) { (void)l; (void)w; (void)r; abort(); }
#ifndef _WIN32
static void io_cb(struct ev_loop *l, ev_io *w, int r) { (void)l; (void)w; (void)r; abort(); }
#endif

static double run_passes(struct ev_loop *l, long passes) {
    long   i;
    double t0;
    for (i = 0; i < 1000; ++i) ev_run(l, EVRUN_NOWAIT); /* warm */
    t0 = now_ns();
    for (i = 0; i < passes; ++i) ev_run(l, EVRUN_NOWAIT);
    return (now_ns() - t0) / (double) passes;
}

static double run_passes_set(struct ev_loop *l, long passes, int free_sample) {
    long      i;
    double    t0;
    ev_tstamp sample = ev_clock_now();
    for (i = 0; i < 1000; ++i) { ev_now_set(l, ev_clock_now()); ev_run(l, EVRUN_NOWAIT); } /* warm */
    t0 = now_ns();
    if (free_sample)
        for (i = 0; i < passes; ++i) { sample += 1e-9; ev_now_set(l, sample); ev_run(l, EVRUN_NOWAIT); }
    else
        for (i = 0; i < passes; ++i) { ev_now_set(l, ev_clock_now()); ev_run(l, EVRUN_NOWAIT); }
    return (now_ns() - t0) / (double) passes;
}

static volatile int gate_sink;
static double run_gate(struct ev_loop *l, long passes) {
    long              i;
    double            t0;
    const int        *timers = ev_timer_count_addr(l);
    const EV_ATOMIC_T *wake  = ev_wake_pending_addr(l);
    ev_tstamp         sample = ev_clock_now();
    int               due    = 0;
    for (i = 0; i < 1000; ++i) due += (*wake != 0) || (*timers && ev_timer_next(l) <= sample); /* warm */
    t0 = now_ns();
    for (i = 0; i < passes; ++i) {
        sample += 1e-9;
        due += (*wake != 0) || (*timers && ev_timer_next(l) <= sample);
    }
    gate_sink = due;
    return (now_ns() - t0) / (double) passes;
}

static int want(int argc, char **argv, const char *shape) {
    int i;
    if (argc <= 2) return 1;
    for (i = 2; i < argc; ++i) if (!strcmp(argv[i], shape)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const long passes = argc > 1 ? atol(argv[1]) : 2000000L;
    printf("qev pass cost (ev %d.%d, backend 0x%x), %ld passes per shape, ns per pass\n",
           ev_version_major(), ev_version_minor(), ev_recommended_backends(), passes);

    if (want(argc, argv, "empty")) {
        struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
        printf("  %-10s %8.1f\n", "empty", run_passes(l, passes));
        ev_loop_destroy(l);
    }
    if (want(argc, argv, "timer")) {
        struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
        ev_timer t; ev_timer_init(&t, never_cb, 3600.0, 0.0); ev_timer_start(l, &t);
        printf("  %-10s %8.1f   (io_count=%u)\n", "timer", run_passes(l, passes), ev_io_count(l));
        ev_timer_stop(l, &t); ev_loop_destroy(l);
    }
#ifndef _WIN32
    if (want(argc, argv, "fd") || want(argc, argv, "fd+timer")) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            if (want(argc, argv, "fd")) {
                struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
                ev_io w; ev_io_init(&w, io_cb, sv[0], EV_READ); ev_io_start(l, &w);
                printf("  %-10s %8.1f   (io_count=%u)\n", "fd", run_passes(l, passes), ev_io_count(l));
                ev_io_stop(l, &w); ev_loop_destroy(l);
            }
            if (want(argc, argv, "fd+timer")) {
                struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
                ev_io w; ev_io_init(&w, io_cb, sv[0], EV_READ); ev_io_start(l, &w);
                ev_timer t; ev_timer_init(&t, never_cb, 3600.0, 0.0); ev_timer_start(l, &t);
                printf("  %-10s %8.1f\n", "fd+timer", run_passes(l, passes));
                ev_timer_stop(l, &t); ev_io_stop(l, &w); ev_loop_destroy(l);
            }
            close(sv[0]); close(sv[1]);
        }
    }
#endif
    if (want(argc, argv, "timer+set") || want(argc, argv, "timer+set0") || want(argc, argv, "gate")) {
        struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
        ev_timer t; ev_timer_init(&t, never_cb, 3600.0, 0.0); ev_timer_start(l, &t);
        if (want(argc, argv, "timer+set"))
            printf("  %-10s %8.1f   (ev_clock_now + ev_now_set + ev_run)\n", "timer+set", run_passes_set(l, passes, 0));
        if (want(argc, argv, "timer+set0"))
            printf("  %-10s %8.1f   (a free sample: ev_now_set + ev_run)\n", "timer+set0", run_passes_set(l, passes, 1));
        if (want(argc, argv, "gate"))
            printf("  %-10s %8.1f   (no ev_run: timer count, ev_timer_next, wake flag)\n", "gate", run_gate(l, passes));
        ev_timer_stop(l, &t); ev_loop_destroy(l);
    }
    if (want(argc, argv, "arm") || want(argc, argv, "arm+now") || want(argc, argv, "now")) {
        struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
        ev_timer t; ev_timer_init(&t, never_cb, 0.5, 0.0);
        long i; double t0;
        ev_run(l, EVRUN_NOWAIT);
        if (want(argc, argv, "arm")) {
            t0 = now_ns();
            for (i = 0; i < passes; ++i) { ev_timer_start(l, &t); ev_timer_stop(l, &t); }
            printf("  %-10s %8.1f   (ev_timer_start + ev_timer_stop)\n", "arm", (now_ns() - t0) / (double) passes);
        }
        if (want(argc, argv, "arm+now")) {
            t0 = now_ns();
            for (i = 0; i < passes; ++i) { ev_now_update(l); ev_timer_start(l, &t); ev_timer_stop(l, &t); }
            printf("  %-10s %8.1f   (ev_now_update + start + stop)\n", "arm+now", (now_ns() - t0) / (double) passes);
        }
        if (want(argc, argv, "now")) {
            t0 = now_ns();
            for (i = 0; i < passes; ++i) ev_now_update(l);
            printf("  %-10s %8.1f   (ev_now_update)\n", "now", (now_ns() - t0) / (double) passes);
        }
        ev_loop_destroy(l);
    }
    return 0;
}
