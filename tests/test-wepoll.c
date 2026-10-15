/*
 * qev — wepoll backend test (Windows only).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The bundled wepoll is the fork's headline feature -- a real epoll on Windows
 * where upstream libev only ever offered select -- and until this file it had
 * no dedicated test: the watcher suite runs its io cases behind #ifndef _WIN32.
 * Seven cases over native winsock SOCKETs, driven under watchdogs:
 *
 *   1. EV_READ delivery through wepoll on a loopback TCP pair.
 *   2. THE REGRESSION THAT MOTIVATED THE FORK: widening a live watcher's
 *      interest set must reach the backend -- both through ev_io_modify() (the
 *      in-place EPOLL_CTL_MOD qb's ev::io::set(events) uses) and through the
 *      stop / set / start cycle. Upstream's ev_io_modify() was a macro writing
 *      w->events in memory and never re-registering with wepoll, so a keep-alive
 *      that widened its interest set never learned the socket was writable
 *      (fixed in 87bb2e03).
 *   3. EPOLLRDHUP half-close: the peer's shutdown(SD_SEND) surfaces as
 *      EV_READ with recv() == 0.
 *   4. 64 concurrent pairs: every watcher fires (the SOCKET<->fd registry
 *      scales past a handful).
 *   5. An EV_READ watcher on a listening socket fires on connect.
 *   6. EVRUN_NOPOLL over a readable SOCKET (QB-191): a NOWAIT|NOPOLL pass does
 *      not call wepoll and delivers nothing, the next plain pass delivers and
 *      the fed count says the poll found one fd, a blocking pass ignores the
 *      flag. The IOCP wait is what the flag saves on Windows (~245 of an io
 *      pass's 275 ns), so this is the platform whose skip matters most; the
 *      POSIX twin of the case lives in test-loops.c over a pipe.
 *   7. Several watchers on one SOCKET (QB-317): stopping one leaves the other
 *      registered with wepoll, and the last one stopped and restarted before
 *      the next pass is registered again.
 *
 * HOW A SOCKET REACHES THE LOOP, AND WHY EVERY VERDICT CHECKS EV_ERROR. A win32
 * ev_io names its socket through ev_io_init_sock / ev_io_set_sock (the handle
 * member); ev_io_start acquires a small fd for it in the SOCKET<->fd registry
 * and ev_io_stop releases it, so a callback reads its socket from w->handle,
 * never from w->fd. Until QB-194 this file wrote ev_io_init(&w, cb, (int)sock,
 * EV_READ) -- a raw SOCKET as fd, registered nowhere -- and every one of its
 * five cases was GREEN: the registry answered INVALID_SOCKET, wepoll's
 * EPOLL_CTL_ADD failed, fd_kill() stopped the watcher and fed it
 * EV_ERROR | EV_READ | EV_WRITE, and a callback that did not look at revents
 * took the kill for a delivery (recv() on the raw value still worked, since the
 * kill left the socket itself intact). Measured on MSVC 19.51: revents
 * 0x80000003 in every case, the loop's io count 0 afterwards, the backend never
 * consulted. The first case built for QB-191 -- which asks whether a pass
 * LOOKED, so it counts what wepoll fed -- is what exposed it. So every callback
 * here records the revents it was given, and no verdict passes with EV_ERROR
 * in it: a kill is a failure, never a delivery.
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

/* A delivery is a callback with EV_ERROR absent: fd_kill's EV_ERROR | EV_READ | EV_WRITE is
   what a mis-registered socket produces, and it must never read as one. */
#define DELIVERED(revents, mask) (((revents) & EV_ERROR) == 0 && ((revents) & (mask)) != 0)

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

/* AFD does not make a loopback byte readable synchronously with the send: wait until the
   kernel reports the socket readable BEFORE a pass whose verdict depends on it, so what the
   check asserts is whether that pass LOOKED, never whether the byte had arrived yet. */
static int
wait_readable(SOCKET s) {
    fd_set rs; struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
    FD_ZERO(&rs); FD_SET(s, &rs);
    return select(0, &rs, NULL, NULL, &tv) == 1;
}

/* ---- 1. plain EV_READ through wepoll ---- */
static int rd_hit, rd_revents;
static void rd_cb(struct ev_loop *l, ev_io *w, int r) { (void)w; rd_revents = r; rd_hit = 1; ev_break(l, EVBREAK_ALL); }
static void test_read(void) {
    rd_hit = 0; rd_revents = 0;
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("wepoll EV_READ", "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
    ev_io w; ev_io_init_sock(&w, rd_cb, b, EV_READ); ev_io_start(l, &w);
    OK(ev_io_count(l) == 1 && w.fd >= 0 && (SOCKET)w.handle == b,
       "ev_io_start on a native SOCKET acquires a registry fd and keeps the handle");
    send(a, "x", 1, 0);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(rd_hit == 1 && DELIVERED(rd_revents, EV_READ), "wepoll delivers EV_READ on a native SOCKET (no EV_ERROR)");
    OK(ev_io_count(l) == 1, "and the watcher is still active afterwards: it was delivered to, not killed");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}

/* ---- 2. the interest-set modification reaches the backend ---- */
static int mod_phase, mod_write_seen, mod_error, mod_use_cycle;
static void mod_cb(struct ev_loop *l, ev_io *w, int r) {
    if (r & EV_ERROR) { mod_error = 1; ev_break(l, EVBREAK_ALL); return; }
    if (mod_phase == 0 && (r & EV_READ)) {
        char buf[8]; recv((SOCKET)w->handle, buf, sizeof buf, 0);
        mod_phase = 1;
        /* Widen the interest set on a LIVE watcher -- the historical bug: this
           re-registration never reached wepoll, so EV_WRITE never arrived. */
        if (mod_use_cycle) {
            ev_io_stop(l, w);
            ev_io_set_sock(w, w->handle, EV_READ | EV_WRITE);
            ev_io_start(l, w);
        } else
            ev_io_modify(l, w, EV_READ | EV_WRITE);
        return;
    }
    if (mod_phase == 1 && (r & EV_WRITE)) { mod_write_seen = 1; ev_break(l, EVBREAK_ALL); }
}
static void modify_once(int use_cycle, const char *name) {
    mod_phase = 0; mod_write_seen = 0; mod_error = 0; mod_use_cycle = use_cycle;
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP(name, "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
    ev_io w; ev_io_init_sock(&w, mod_cb, b, EV_READ); ev_io_start(l, &w);
    send(a, "x", 1, 0);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(mod_write_seen == 1 && !mod_error, name);
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}
static void test_modify_regression(void) {
    modify_once(0, "widening a live watcher's interest set with ev_io_modify reaches wepoll (the 87bb2e03 regression)");
    modify_once(1, "widening it through stop / ev_io_set_sock / start reaches wepoll too");
}

/* ---- 3. half-close surfaces as EV_READ + recv()==0 ---- */
static int hup_eof, hup_error;
static void hup_cb(struct ev_loop *l, ev_io *w, int r) {
    if (r & EV_ERROR) { hup_error = 1; ev_break(l, EVBREAK_ALL); return; }
    char buf[8];
    int  n = recv((SOCKET)w->handle, buf, sizeof buf, 0);
    if (n == 0) { hup_eof = 1; ev_break(l, EVBREAK_ALL); }
}
static void test_halfclose(void) {
    hup_eof = 0; hup_error = 0;
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("half-close", "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
    ev_io w; ev_io_init_sock(&w, hup_cb, b, EV_READ); ev_io_start(l, &w);
    shutdown(a, SD_SEND);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(hup_eof == 1 && !hup_error, "peer half-close arrives as EV_READ with recv() == 0 (RDHUP path)");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}

/* ---- 4. sixty-four concurrent watchers ---- */
#define NPAIR 64
static int  many_seen, many_error;
static void many_cb(struct ev_loop *l, ev_io *w, int r) {
    if (r & EV_ERROR) { ++many_error; ev_break(l, EVBREAK_ALL); return; }
    char buf[8]; recv((SOCKET)w->handle, buf, sizeof buf, 0);
    ev_io_stop(l, w);
    if (++many_seen == NPAIR) ev_break(l, EVBREAK_ALL);
}
static void test_many_sockets(void) {
    many_seen = 0; many_error = 0;
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
    struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
    for (int i = 0; i < NPAIR; ++i) {
        ev_io_init_sock(&ws[i], many_cb, bs[i], EV_READ);
        ev_io_start(l, &ws[i]);
        send(as[i], "y", 1, 0);
    }
    ev_timer wd; ev_timer_init(&wd, wd_cb, 5.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(many_seen == NPAIR && many_error == 0, "64 concurrent SOCKET watchers all fire (registry scales)");
    ev_loop_destroy(l);
    for (int i = 0; i < NPAIR; ++i) { closesocket(as[i]); closesocket(bs[i]); }
}

/* ---- 5. a listener watcher fires on connect ---- */
static int acc_hit, acc_error;
static void acc_cb(struct ev_loop *l, ev_io *w, int r) {
    if (r & EV_ERROR) { acc_error = 1; ev_break(l, EVBREAK_ALL); return; }
    SOCKET c = accept((SOCKET)w->handle, NULL, NULL);
    if (c != INVALID_SOCKET) { acc_hit = 1; closesocket(c); }
    ev_break(l, EVBREAK_ALL);
}
static void test_accept(void) {
    acc_hit = 0; acc_error = 0;
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
    struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
    ev_io w; ev_io_init_sock(&w, acc_cb, ls, EV_READ); ev_io_start(l, &w);
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    connect(c, (struct sockaddr *)&sa, sizeof sa);
    ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    OK(acc_hit == 1 && !acc_error, "EV_READ on a listening SOCKET fires on connect");
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(c); closesocket(ls);
}

/* ---- 6. EVRUN_NOPOLL: the embedder owns the io cadence (QB-191) ---- */
static int nopoll_hits, nopoll_error;
static void nopoll_cb(struct ev_loop *l, ev_io *w, int r) {
    (void)l;
    if (r & EV_ERROR) { ++nopoll_error; return; }
    char buf[8];
    recv((SOCKET)w->handle, buf, sizeof buf, 0); /* drain, so the byte is counted once */
    ++nopoll_hits;
}
static void test_nopoll_socket(void) {
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("EVRUN_NOPOLL over a readable SOCKET", "loopback pair failed"); return; }
    struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
    ev_io w; ev_io_init_sock(&w, nopoll_cb, b, EV_READ); ev_io_start(l, &w);
    OK(*ev_io_count_addr(l) == 1, "the inline io count follows ev_io_start on a SOCKET");
    nopoll_hits = 0; nopoll_error = 0;
    send(a, "x", 1, 0);
    if (!wait_readable(b)) { SKIP("EVRUN_NOPOLL over a readable SOCKET", "loopback byte not readable within 1 s"); }
    else {
        const unsigned int fed0 = *ev_io_fed_addr(l);
        ev_run(l, EVRUN_NOWAIT | EVRUN_NOPOLL);
        OK(nopoll_hits == 0 && nopoll_error == 0 && *ev_io_fed_addr(l) == fed0,
           "a readable SOCKET is NOT delivered by a NOWAIT|NOPOLL pass: wepoll was not asked, the fed count did not move");
        ev_run(l, EVRUN_NOWAIT);
        OK(nopoll_hits == 1 && nopoll_error == 0 && *ev_io_fed_addr(l) == fed0 + 1,
           "the next NOWAIT pass without the flag delivers it, and the fed count says the poll found one fd");
        ev_run(l, EVRUN_NOWAIT);
        OK(*ev_io_fed_addr(l) == fed0 + 1, "a poll that finds nothing leaves the fed count alone");
        send(a, "y", 1, 0);
        {
            ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
            ev_run(l, EVRUN_ONCE | EVRUN_NOPOLL);
            ev_timer_stop(l, &wd);
        }
        OK(nopoll_hits == 2 && nopoll_error == 0, "a blocking pass ignores the flag: with a SOCKET to wait on, the IOCP wait is the wake");
    }
    ev_io_stop(l, &w); ev_loop_destroy(l);
    closesocket(a); closesocket(b);
}

/* ---- 7. several watchers on one SOCKET: a stop keeps the others registered (QB-317) ----
   A SOCKET may carry several ev_io watchers; they share its registry fd and ANFD, and fd_reify
   tells wepoll only the AGGREGATE mask, and only when it changes. ev_io_stop deleted the socket's
   wepoll registration unconditionally: stopping one of two READ watchers left the aggregate READ,
   fd_reify saw nothing to change, and the survivor stayed active in the loop and unknown to wepoll
   -- no event, ever. The same desynchronisation took the last watcher stopped and started again
   (no ev_io_set) before the next pass: the registration was gone, the mask unchanged, nothing
   re-added it. Every verdict is a NEW byte sent after the stop, readable before the run, delivered
   under a 2 s watchdog; the callbacks only count (two of them reading one byte could block) and the
   round drains afterwards. A narrowed aggregate (READ|WRITE -> READ) is fd_reify's to MOD, and is
   kept as the control that worked before. The final stop's eager DEL stays: case 2's
   stop / set / start covers it. */
static int dw_hits[2], dw_error, dw_want;
static void dw_cb(struct ev_loop *l, ev_io *w, int r) {
    int i = (int) (intptr_t) w->data;
    if (r & EV_ERROR) { dw_error = 1; ev_break(l, EVBREAK_ALL); return; }
    if (r & EV_READ) ++dw_hits[i];
    if ((!(dw_want & 1) || dw_hits[0]) && (!(dw_want & 2) || dw_hits[1])) ev_break(l, EVBREAK_ALL);
}
/* One byte from a to b, then a blocking run until every watcher in `want` (bit i = dw watcher i)
   has seen EV_READ or the watchdog fires; returns 0 when b was readable before the run. */
static int dw_round(struct ev_loop *l, SOCKET a, SOCKET b, int want) {
    char     buf[8];
    ev_timer wd;
    dw_hits[0] = dw_hits[1] = 0; dw_error = 0; dw_want = want;
    send(a, "x", 1, 0);
    if (!wait_readable(b)) return -1;
    ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
    ev_run(l, 0);
    ev_timer_stop(l, &wd);
    recv(b, buf, 1, 0); /* drain: b was readable, so this cannot block */
    return 0;
}
static void test_shared_socket(void) {
    SOCKET a, b;
    if (tcp_pair(&a, &b)) { SKIP("several watchers on one SOCKET", "loopback pair failed"); return; }
    {
        struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
        ev_io wa, wb;
        ev_io_init_sock(&wa, dw_cb, b, EV_READ); wa.data = (void *) (intptr_t) 0; ev_io_start(l, &wa);
        ev_io_init_sock(&wb, dw_cb, b, EV_READ); wb.data = (void *) (intptr_t) 1; ev_io_start(l, &wb);
        OK(dw_round(l, a, b, 3) == 0 && dw_hits[0] && dw_hits[1] && !dw_error && wa.fd == wb.fd,
           "two READ watchers on one SOCKET share its registry fd and both see a byte through wepoll");
        ev_io_stop(l, &wa);
        OK(dw_round(l, a, b, 2) == 0 && dw_hits[1] && !dw_error && ev_is_active(&wb),
           "after one of them stops, the other still sees the next byte: the shared registration was not deleted under it (QB-317)");
        ev_io_stop(l, &wb);
        ev_loop_destroy(l);
    }
    {
        struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
        ev_io wa, wb;
        ev_io_init_sock(&wa, dw_cb, b, EV_WRITE); wa.data = (void *) (intptr_t) 0; ev_io_start(l, &wa);
        ev_io_init_sock(&wb, dw_cb, b, EV_READ);  wb.data = (void *) (intptr_t) 1; ev_io_start(l, &wb);
        ev_run(l, EVRUN_NOWAIT);                 /* registered as READ|WRITE */
        ev_io_stop(l, &wa);
        OK(dw_round(l, a, b, 2) == 0 && dw_hits[1] && !dw_error,
           "control: when the stop narrows the aggregate (READ|WRITE -> READ), the survivor still sees the next byte");
        ev_io_stop(l, &wb);
        ev_loop_destroy(l);
    }
    {
        struct ev_loop *l = ev_loop_new(EVBACKEND_EPOLL);
        ev_io wc;
        ev_io_init_sock(&wc, dw_cb, b, EV_READ); wc.data = (void *) (intptr_t) 0; ev_io_start(l, &wc);
        dw_round(l, a, b, 1);                    /* registered and delivered once */
        ev_io_stop(l, &wc);                      /* the last watcher: the eager DEL ... */
        ev_io_start(l, &wc);                     /* ... and back before any pass, without ev_io_set_sock */
        OK(dw_round(l, a, b, 1) == 0 && dw_hits[0] && !dw_error,
           "the last watcher stopped and started again before the next pass still sees the next byte: the eager DEL is re-added (QB-317)");
        ev_io_stop(l, &wc);
        ev_loop_destroy(l);
    }
    closesocket(a); closesocket(b);
}

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("FAIL WSAStartup\n");
        return 1;
    }
    printf("qev wepoll coverage (ev %d.%d, backends=0x%x)\n",
           ev_version_major(), ev_version_minor(), ev_supported_backends());
    if (!(ev_supported_backends() & EVBACKEND_EPOLL)) {
        printf("FAIL wepoll/epoll backend unavailable\n");
        WSACleanup();
        return 1;
    }
    struct ev_loop *probe = ev_loop_new(EVBACKEND_EPOLL);
    if (!probe || ev_backend(probe) != EVBACKEND_EPOLL) {
        printf("FAIL explicit wepoll/epoll loop selected another backend\n");
        if (probe) ev_loop_destroy(probe);
        WSACleanup();
        return 1;
    }
    ev_loop_destroy(probe);

    test_read();
    test_modify_regression();
    test_halfclose();
    test_many_sockets();
    test_accept();
    test_nopoll_socket();
    test_shared_socket();

    printf("\n== wepoll: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);
    WSACleanup();

    /* All seventeen checks are unconditional on Windows (read 3, modify 2, half-close 1,
       64 pairs 1, accept 1, NOPOLL 5, shared socket 4 -- the first five cases' checks measured
       nothing until QB-194, see the header); a run that skipped its way to zero measured nothing
       and must not read as a pass. */
    if (g_run < 17) {
        printf("== FAIL: only %d checks ran; all 17 are unconditional on Windows ==\n", g_run);
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
