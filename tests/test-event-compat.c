/*
 * qev — the libevent compatibility layer (event.c), driven.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The layer is built only with QB_EV_LIBEVENT_COMPAT=ON, and nothing ran it until this file:
 * scripts/test-build-matrix.sh configures that profile, links this program against it and runs
 * it. It holds libevent's removal contract -- after event_del, the callback does not run, however
 * the event was made pending -- which event_del broke for a timeout made pending by event_active:
 * it stopped the timer only when the timer was ACTIVE, and an activation feeds it without starting
 * it, so the queued callback ran on the next pass anyway, after the removal (Huly QB-712). The
 * same held for a pure timeout event activated as readable: its io watcher was never stopped.
 * Every activation kind is checked deleted and, as a control, kept.
 */
#include <qev/event.h>
#include <signal.h>
#include <stdio.h>

static int g_fail = 0, g_run = 0;
#define OK(cond, name) do { ++g_run; if (cond) printf("  PASS  %s\n", (name)); else { printf("  FAIL  %s\n", (name)); ++g_fail; } } while (0)

static int   calls;
static short last_res;
static void  cb(int fd, short res, void *arg) { (void)fd; (void)arg; ++calls; last_res = res; }

/* One non-blocking pass: whatever is pending runs now or not at all. */
static void pass(void) { event_loop(EVLOOP_NONBLOCK); }

/* `kind` is how the event was set up, `res` what event_active makes pending. */
static void activation(const char *kind, int fd, short events, short res, int del, const char *name) {
    struct event ev;
    int          before, after;
    event_set(&ev, fd, events, cb, NULL);
    calls = 0; last_res = 0;
    event_active(&ev, res, 1);
    before = event_pending(&ev, (short) (EV_TIMEOUT | EV_READ | EV_WRITE | EV_SIGNAL), NULL) != 0;
    if (del) event_del(&ev);
    after = event_pending(&ev, (short) (EV_TIMEOUT | EV_READ | EV_WRITE | EV_SIGNAL), NULL) != 0;
    pass();
    printf("        %s, activated 0x%x%s: pending before=%d after=%d, callback ran %d time(s)\n",
           kind, (unsigned) res, del ? ", then event_del" : "", before, after, calls);
    /* Each deleted case has a kept twin below that proves the activation was real. */
    if (del)
        OK(!after && calls == 0, name);
    else
        OK(calls == 1 && (last_res & res) == res, name);
    if (!del) event_del(&ev);
}

int main(void) {
    printf("qev libevent compatibility layer (%s %s)\n", event_get_method(), event_get_version());
    event_init();

    activation("timeout event", -1, 0, EV_TIMEOUT, 1,
               "event_del cancels a timeout activated by event_active: no callback after removal (QB-712)");
    activation("timeout event", -1, 0, EV_TIMEOUT, 0,
               "control: the same activation, kept, runs the callback once with EV_TIMEOUT");
    activation("timeout event", -1, 0, EV_READ, 1,
               "event_del cancels a pure timeout event activated as readable: no callback after removal (QB-712)");
    activation("timeout event", -1, 0, EV_READ, 0,
               "control: the same readable activation, kept, runs the callback once with EV_READ");
    activation("io event", -1, EV_READ, EV_READ, 1,
               "control: event_del cancels an activated io event (its io watcher was always stopped)");
    activation("io event", -1, EV_READ, EV_READ, 0,
               "control: the same io activation, kept, runs the callback once with EV_READ");
    activation("signal event", SIGINT, EV_SIGNAL, EV_SIGNAL, 1,
               "control: event_del cancels an activated signal event");
    activation("signal event", SIGINT, EV_SIGNAL, EV_SIGNAL, 0,
               "control: the same signal activation, kept, runs the callback once with EV_SIGNAL");

    {   /* a timeout really armed, then deleted: the ordinary removal */
        struct event   ev;
        struct timeval tv = {0, 0};          /* due at once (the layer turns 0 into 1 us) */
        evtimer_set(&ev, cb, NULL);
        calls = 0;
        evtimer_add(&ev, &tv);
        event_del(&ev);
        pass();
        OK(calls == 0 && !evtimer_pending(&ev, NULL), "control: an armed timeout deleted before it fires never runs");
    }

    printf("\n== event-compat: %d run, %d failed ==\n", g_run, g_fail);
    if (g_run < 9) {
        printf("== FAIL: only %d checks ran; all 9 are unconditional ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}
