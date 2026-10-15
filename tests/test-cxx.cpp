/*
 * qev — the C++ wrapper (ev++.h), driven.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The wrapper is all templates, and a template member that nothing instantiates is never
 * compiled: three of its defects lived for that reason alone. Calling a watcher directly,
 * `w (revents)`, passed the watcher where the callback takes the loop (Huly QB-312);
 * `ev::embed::start (loop)` resolved to the base's `set (loop)` and moved the watcher to the
 * loop it was meant to embed (QB-313); and under EV_MULTIPLICITY=0, the single-loop embedding
 * profile docs/ev.pod says the wrapper supports, the three callback thunks named a `loop`
 * parameter that profile does not have, so the header did not compile at all (QB-711).
 *
 * scripts/test-build-matrix.sh builds this file twice: against the library (EV_MULTIPLICITY=1,
 * every watcher family) and with ev.c compiled in under EV_MULTIPLICITY=0 (QEV_TEST_EMBEDDED).
 * Every callback form is instantiated and driven by the loop, then invoked directly; the loop
 * a direct invocation passes is checked against the watcher's own, on two loops.
 */
#ifdef QEV_TEST_EMBEDDED
# include "ev++.h"
#else
# include <qev/ev++.h>
#endif
#include <cstdio>

#if !defined(_WIN32) && defined(EV_EMBED_ENABLE) && EV_EMBED_ENABLE
# include <unistd.h>
#endif

static int g_fail = 0, g_run = 0, g_skip = 0;
#define OK(cond, name)   do { ++g_run; if (cond) std::printf("  PASS  %s\n", (name)); else { std::printf("  FAIL  %s\n", (name)); ++g_fail; } std::fflush(stdout); } while (0)
#define SKIP(name, why)  do { ++g_skip; std::printf("  SKIP  %s (%s)\n", (name), (why)); } while (0)

/* A watcher's constructor takes its loop only when there is more than one. */
#if EV_MULTIPLICITY
# define ON(loop) (loop)
#else
# define ON(loop)
#endif

/* ---- what the callbacks saw ---- */
struct seen_t {
    int             calls;
    void           *watcher;
    int             revents;
    struct ev_loop *loop;   /* raw C callbacks only: the wrapper's thunks do not hand it on */
};
static seen_t seen;
static void forget() { seen.calls = 0; seen.watcher = 0; seen.revents = 0; seen.loop = 0; }

static void fn_cb(ev::timer &w, int revents) { ++seen.calls; seen.watcher = &w; seen.revents = revents; }

struct target {
    void method(ev::timer &w, int revents) { ++seen.calls; seen.watcher = &w; seen.revents = revents; }
    void noargs() { ++seen.calls; }
    void operator()(ev::timer &w, int revents) { ++seen.calls; seen.watcher = &w; seen.revents = revents; }
};

#if EV_MULTIPLICITY
static void raw_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    ++seen.calls; seen.watcher = w; seen.revents = revents; seen.loop = loop;
}
#else
static void raw_cb(ev_timer *w, int revents) { ++seen.calls; seen.watcher = w; seen.revents = revents; }
#endif

/* ---- each callback form: driven by the loop, then called directly ---- */
static void drive(ev::loop_ref loop, ev::timer &t) {
    /* A zero-delay timer, fired by the loop through the same thunk a direct call uses. A deadline
       fires once it is strictly behind the loop's clock, so a pass may come too early to see it. */
    int pass;
    t.start(0., 0.);
    for (pass = 0; pass < 100000 && seen.calls == 0; ++pass) loop.run(ev::NOWAIT);
    t.stop();
}

static void test_callback_forms(ev::loop_ref loop) {
    target    k;
    ev::timer t ON(loop);

    t.set<fn_cb>();
    forget(); drive(loop, t);
    OK(seen.calls == 1 && seen.watcher == &t && seen.revents == ev::TIMER, "function callback: the loop invokes it with the watcher and EV_TIMER");
    forget(); t(ev::READ);
    OK(seen.calls == 1 && seen.watcher == &t && seen.revents == ev::READ, "function callback: w (revents) invokes it directly with the watcher and those revents (QB-312)");

    t.set<target, &target::method>(&k);
    forget(); drive(loop, t);
    OK(seen.calls == 1 && seen.watcher == &t && seen.revents == ev::TIMER, "method callback: the loop invokes it");
    forget(); t(ev::WRITE);
    OK(seen.calls == 1 && seen.watcher == &t && seen.revents == ev::WRITE, "method callback: w (revents) invokes it directly (QB-312)");

    t.set<target, &target::noargs>(&k);
    forget(); drive(loop, t);
    OK(seen.calls == 1, "no-argument method callback: the loop invokes it");
    forget(); t();
    OK(seen.calls == 1, "no-argument method callback: w () invokes it directly (QB-312)");

    t.set(&k);
    forget(); drive(loop, t);
    OK(seen.calls == 1 && seen.watcher == &t && seen.revents == ev::TIMER, "functor callback (operator ()): the loop invokes it");
    forget(); t();
    OK(seen.calls == 1 && seen.watcher == &t && seen.revents == ev::UNDEF, "functor callback: w () invokes it directly with EV_UNDEF, the default (QB-312)");
}

/* ---- a direct call hands the callback the watcher's OWN loop ---- */
static void test_direct_invocation_loop(ev::loop_ref a, ev::loop_ref b) {
#if !EV_MULTIPLICITY
    (void) a; (void) b;   /* one loop: every watcher is on it */
#endif
    ev::timer ta ON(a);
    ev::timer tb ON(b);
    ta.set_(0, raw_cb);
    tb.set_(0, raw_cb);

    forget(); ta(ev::TIMER);
    bool on_a = seen.calls == 1 && seen.watcher == static_cast<ev_timer *>(&ta) && seen.revents == ev::TIMER;
#if EV_MULTIPLICITY
    on_a = on_a && seen.loop == static_cast<struct ev_loop *>(a);
#endif
    forget(); tb(ev::TIMER);
    bool on_b = seen.calls == 1 && seen.watcher == static_cast<ev_timer *>(&tb) && seen.revents == ev::TIMER;
#if EV_MULTIPLICITY
    on_b = on_b && seen.loop == static_cast<struct ev_loop *>(b);
#endif
    OK(on_a && on_b, "a direct call passes the C callback its watcher's loop, the watcher and the revents, on each of two loops (QB-312)");
}

/* ---- ev::embed::start (loop) embeds that loop and stays on its own ---- */
#if defined(EV_EMBED_ENABLE) && EV_EMBED_ENABLE && !defined(_WIN32)
static int  embed_hits;
static void embed_io(ev::io &w, int) { char c; if (read(w.fd, &c, 1) == 1) ++embed_hits; w.loop.break_loop(ev::ALL); }
static void embed_bail(ev::timer &w, int) { w.loop.break_loop(ev::ALL); }

/* One delivery: a byte on the pipe the INNER loop watches is seen by running the OUTER one --
   the embed watcher has no callback, so the outer loop sweeps the inner one itself. */
/* The raw loop a watcher is on (compared as pointers: loop_ref's own operator== takes a pointer to
   const, which leaves a comparison with a plain `struct ev_loop *` ambiguous to some compilers). */
static struct ev_loop *owner(ev::embed &e) { return static_cast<struct ev_loop *>(e.loop); }

static bool embed_delivers(ev::loop_ref outer, int wfd) {
    ev::timer bail ON(outer);
    int before = embed_hits;
    bail.set<embed_bail>();
    bail.start(2., 0.);
    if (write(wfd, "x", 1) == 1) outer.run();
    bail.stop();
    return embed_hits == before + 1;
}

static void test_embed_start() {
    const char *name = "ev::embed::start (loop) embeds that loop, keeps the watcher on its own loop, and delivers through it, across a stop and a second start (QB-313)";
    const char *fresh = "ev::embed::start (loop) on a watcher never configured before embeds that loop (QB-313)";
    struct ev_loop *inner = ev_loop_new(EVFLAG_NOENV | (ev_embeddable_backends() & ev_recommended_backends()));
    int fds[2];
    if (!inner || !(ev_embeddable_backends() & ev_backend(inner))) {
        if (inner) ev_loop_destroy(inner);
        SKIP(name, "no embeddable backend here"); SKIP(fresh, "no embeddable backend here");
        return;
    }
    if (pipe(fds) != 0) { ev_loop_destroy(inner); SKIP(name, "pipe() failed"); SKIP(fresh, "pipe() failed"); return; }
    {
        ev::dynamic_loop outer;
        ev::io           in_io(inner);
        in_io.set<embed_io>();
        in_io.start(fds[0], ev::READ);

        /* `other` is configured first, so a start that retargets the OWNER is observed as such
           instead of reading a field ev_init leaves undefined. */
        {
            ev::embed e(outer);
            e.set_embed(inner);
            e.start(inner);
            bool kept = owner(e) == static_cast<struct ev_loop *>(outer) && e.other == inner && e.is_active();
            bool first = kept && embed_delivers(outer, fds[1]);
            e.stop();
            e.start(inner);
            bool again = owner(e) == static_cast<struct ev_loop *>(outer) && e.is_active() && embed_delivers(outer, fds[1]);
            e.stop();
            OK(kept && first && again, name);
        }
        {
            ev::embed e(outer);
            e.start(inner);
            bool ok = owner(e) == static_cast<struct ev_loop *>(outer) && e.other == inner && embed_delivers(outer, fds[1]);
            e.stop();
            OK(ok, fresh);
        }
        in_io.stop();
    }
    close(fds[0]); close(fds[1]);
    ev_loop_destroy(inner);
}
#endif

int main() {
    std::printf("qev C++ wrapper (ev %d.%d, EV_MULTIPLICITY=%d, backends=0x%x)\n",
                ev::version_major(), ev::version_minor(), (int) EV_MULTIPLICITY, ev::supported_backends());
    {
#if EV_MULTIPLICITY
        ev::dynamic_loop a;
        ev::dynamic_loop b;
        test_callback_forms(a);
        test_direct_invocation_loop(a, b);
#else
        ev::default_loop a;
        test_callback_forms(a);
        test_direct_invocation_loop(a, a);
#endif
    }
#if defined(EV_EMBED_ENABLE) && EV_EMBED_ENABLE && !defined(_WIN32)
    test_embed_start();
#elif EV_MULTIPLICITY
    SKIP("ev::embed::start (loop) (QB-313)", "embed watchers compiled out, or not embeddable on this platform");
#endif

    std::printf("\n== cxx: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);
    /* Nine checks need nothing but a loop and are in every profile on every platform. */
    if (g_run < 9) {
        std::printf("== FAIL: only %d checks ran; at least 9 are unconditional ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}
