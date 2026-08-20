/*
 * qev — wepoll backend test (Windows only).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The bundled wepoll is the fork's headline feature -- a real epoll on Windows
 * where upstream libev only ever offered select -- and until this file it had
 * no dedicated test: the watcher suite runs its io cases behind #ifndef _WIN32.
 * Five cases over native winsock SOCKETs, driven under watchdogs:
 *
 *   1. EV_READ delivery through wepoll on a loopback TCP pair.
 *   2. THE REGRESSION THAT MOTIVATED THE FORK: stop / ev_io_set(READ|WRITE) /
 *      start on a live watcher must reach the backend. Upstream's
 *      ev_io_modify() was a macro writing w->events in memory and never
 *      re-registering with wepoll, so a keep-alive that widened its interest
 *      set never learned the socket was writable (fixed in e8090ecc).
 *   3. EPOLLRDHUP half-close: the peer's shutdown(SD_SEND) surfaces as
 *      EV_READ with recv() == 0.
 *   4. 64 concurrent pairs: every watcher fires (the SOCKET<->fd registry
 *      scales past a handful).
 *   5. An EV_READ watcher on a listening socket fires on connect.
 *
 * Registered by CMake only on WIN32 -- a green "wepoll" tick on a platform
 * that never runs wepoll would be a lie, not a skip.
 */
#ifdef _WIN32

#include <qev/ev.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static int g_fail = 0, g_run = 0, g_skip = 0;
#define OK(cond, name)   do { ++g_run; if (cond) printf("  PASS  %s\n", (name)); else { printf("  FAIL  %s\n", (name)); ++g_fail; } } while (0)
#define SKIP(name, why)  do { ++g_skip; printf("  SKIP  %s (%s)\n", (name), (why)); } while (0)

static void wd_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ev_break(l, EVBREAK_ALL); }

/* Build a connected loopback TCP pair out of native SOCKETs. Returns 0 on success. */
static int
tcp_pair(SOCKET *a, SOCKET *b) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    int len = sizeof sa;
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 1) ||
        getsockname(ls, (struct sockaddr *)&sa, &len)) { closesocket(ls); return -1; }
    *a = socket(AF_INET, SOCK_STREAM, 0);
    if (*a == INVALID_SOCKET) { closesocket(ls); return -1; }
    if (connect(*a, (struct sockaddr *)&sa, sizeof sa)) { closesocket(*a); closesocket(ls); return -1; }
    *b = accept(ls, NULL, NULL);
    closesocket(ls);
    if (*b == INVALID_SOCKET) { closesocket(*a); return -1; }
    return 0;
}

/* ---- 1. plain EV_READ through wepoll ---- */
static int rd_hit;
static void rd_cb(struct ev_loop *l, ev_io *w, int r) { (void)w; (void)r; rd_hit = 1; ev_break(l, EVBREAK_ALL); }
static void test_read(void) {
    rd_hit = 0;
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("wepoll EV_READ", "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_io w; ev_io_init(&w, rd_cb, (int)b, EV_READ); ev_io_start(l, &w);
    send(a, "x", 1, 0);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(rd_hit == 1, "wepoll delivers EV_READ on a native SOCKET");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}

/* ---- 2. the interest-set modification reaches the backend ---- */
static int mod_phase, mod_write_seen;
static void mod_cb(struct ev_loop *l, ev_io *w, int r) {
    if (mod_phase == 0 && (r & EV_READ)) {
        char buf[8]; recv((SOCKET)w->fd, buf, sizeof buf, 0);
        mod_phase = 1;
        /* Widen the interest set on a LIVE watcher -- the historical bug: this
           re-registration never reached wepoll, so EV_WRITE never arrived. */
        ev_io_stop(l, w);
        ev_io_set(w, w->fd, EV_READ | EV_WRITE);
        ev_io_start(l, w);
        return;
    }
    if (mod_phase == 1 && (r & EV_WRITE)) { mod_write_seen = 1; ev_break(l, EVBREAK_ALL); }
}
static void test_modify_regression(void) {
    mod_phase = 0; mod_write_seen = 0;
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("interest-set modify", "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_io w; ev_io_init(&w, mod_cb, (int)b, EV_READ); ev_io_start(l, &w);
    send(a, "x", 1, 0);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(mod_write_seen == 1,
       "widening a live watcher's interest set reaches wepoll (the e8090ecc regression)");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}

/* ---- 3. half-close surfaces as EV_READ + recv()==0 ---- */
static int hup_eof;
static void hup_cb(struct ev_loop *l, ev_io *w, int r) {
    (void)r;
    char buf[8];
    int  n = recv((SOCKET)w->fd, buf, sizeof buf, 0);
    if (n == 0) { hup_eof = 1; ev_break(l, EVBREAK_ALL); }
}
static void test_halfclose(void) {
    hup_eof = 0;
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("half-close", "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_io w; ev_io_init(&w, hup_cb, (int)b, EV_READ); ev_io_start(l, &w);
    shutdown(a, SD_SEND);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(hup_eof == 1, "peer half-close arrives as EV_READ with recv() == 0 (RDHUP path)");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}

/* ---- 4. sixty-four concurrent watchers ---- */
#define NPAIR 64
static int  many_seen;
static void many_cb(struct ev_loop *l, ev_io *w, int r) {
    (void)r;
    char buf[8]; recv((SOCKET)w->fd, buf, sizeof buf, 0);
    ev_io_stop(l, w);
    if (++many_seen == NPAIR) ev_break(l, EVBREAK_ALL);
}
static void test_many_sockets(void) {
    many_seen = 0;
    SOCKET as[NPAIR], bs[NPAIR];
    static ev_io ws[NPAIR];
    int made = 0;
    for (; made < NPAIR; ++made)
        if (tcp_pair(&as[made], &bs[made])) break;
    if (made < NPAIR) {
        for (int i = 0; i < made; ++i) { closesocket(as[i]); closesocket(bs[i]); }
        SKIP("64 concurrent watchers", "could not build all loopback pairs");
        return;
    }
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    for (int i = 0; i < NPAIR; ++i) {
        ev_io_init(&ws[i], many_cb, (int)bs[i], EV_READ);
        ev_io_start(l, &ws[i]);
        send(as[i], "y", 1, 0);
    }
    ev_timer wd; ev_timer_init(&wd, wd_cb, 5.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(many_seen == NPAIR, "64 concurrent SOCKET watchers all fire (registry scales)");
    ev_loop_destroy(l);
    for (int i = 0; i < NPAIR; ++i) { closesocket(as[i]); closesocket(bs[i]); }
}

/* ---- 5. a listener watcher fires on connect ---- */
static int acc_hit;
static void acc_cb(struct ev_loop *l, ev_io *w, int r) {
    (void)r;
    SOCKET c = accept((SOCKET)w->fd, NULL, NULL);
    if (c != INVALID_SOCKET) { acc_hit = 1; closesocket(c); }
    ev_break(l, EVBREAK_ALL);
}
static void test_accept(void) {
    acc_hit = 0;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    int len = sizeof sa;
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&sa, sizeof sa) ||
        listen(ls, 1) || getsockname(ls, (struct sockaddr *)&sa, &len)) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        SKIP("listener watcher", "listen setup failed");
        return;
    }
    struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
    ev_io w; ev_io_init(&w, acc_cb, (int)ls, EV_READ); ev_io_start(l, &w);
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    connect(c, (struct sockaddr *)&sa, sizeof sa);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(acc_hit == 1, "EV_READ on a listening SOCKET fires on connect");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(c); closesocket(ls);
}

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("FAIL WSAStartup\n");
        return 1;
    }
    printf("qev wepoll coverage (ev %d.%d, backends=0x%x)\n",
           ev_version_major(), ev_version_minor(), ev_supported_backends());

    test_read();
    test_modify_regression();
    test_halfclose();
    test_many_sockets();
    test_accept();

    printf("\n== wepoll: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);
    WSACleanup();

    /* All five are unconditional on Windows; a run that skipped its way to zero
       measured nothing and must not read as a pass. */
    if (g_run < 5) {
        printf("== FAIL: only %d checks ran; all 5 are unconditional on Windows ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}

#else  /* !_WIN32 */
/* Never registered by CMake off Windows; this stub only keeps a stray compile honest. */
#include <stdio.h>
int main(void) {
    printf("wepoll is Windows-only; this binary should not have been built here\n");
    return 1;
}
#endif
