/*
 * qb-ev — watcher coverage test.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor).
 *
 * Exercises every libev watcher type enabled in qb-ev: io, timer, periodic,
 * idle, prepare, check, signal, child, stat, embed, fork, async, cleanup, and
 * ev_once. Each test drives a dedicated loop with a watchdog so a non-firing
 * watcher fails (instead of hanging). Exits non-zero if any check fails.
 */
#include <qev/ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <fcntl.h>
# include <signal.h>
# include <sys/socket.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

static int g_fail = 0, g_run = 0, g_skip = 0;
#define OK(cond, name)   do { ++g_run; if (cond) printf("  PASS  %s\n", (name)); else { printf("  FAIL  %s\n", (name)); ++g_fail; } } while (0)
#define SKIP(name, why)  do { ++g_skip; printf("  SKIP  %s (%s)\n", (name), (why)); } while (0)

static void wd_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ev_break(l, EVBREAK_ALL); }

/* ---- timer ---- */
static int timer_hit;
static void timer_cb(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; timer_hit = 1; ev_break(l, EVBREAK_ALL); }
static void test_timer(void) {
  timer_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_timer t; ev_timer_init(&t, timer_cb, 0.01, 0.0); ev_timer_start(l, &t);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(timer_hit, "timer");
  ev_loop_destroy(l);
}

/* ---- periodic ---- */
static int per_hit;
static void per_cb(struct ev_loop *l, ev_periodic *w, int r) { (void)w; (void)r; per_hit = 1; ev_break(l, EVBREAK_ALL); }
static void test_periodic(void) {
  per_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_periodic p; ev_periodic_init(&p, per_cb, ev_time() + 0.01, 0.0, 0); ev_periodic_start(l, &p);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(per_hit, "periodic");
  ev_loop_destroy(l);
}

/* ---- idle ---- */
#if EV_IDLE_ENABLE
static int idle_hit;
static void idle_cb(struct ev_loop *l, ev_idle *w, int r) { (void)r; idle_hit = 1; ev_idle_stop(l, w); ev_break(l, EVBREAK_ALL); }
static void test_idle(void) {
  idle_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_idle id; ev_idle_init(&id, idle_cb); ev_idle_start(l, &id);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(idle_hit, "idle");
  ev_loop_destroy(l);
}
#else
static void test_idle(void) { SKIP("idle", "EV_IDLE_ENABLE is 0 in this build"); }
#endif


/* ---- prepare + check ---- */
#if EV_PREPARE_ENABLE
static int prep_hit, chk_hit;
static void prep_cb(struct ev_loop *l, ev_prepare *w, int r) { (void)l; (void)w; (void)r; prep_hit = 1; }
static void chk_cb(struct ev_loop *l, ev_check *w, int r) { (void)w; (void)r; chk_hit = 1; ev_break(l, EVBREAK_ALL); }
static void test_prepare_check(void) {
  prep_hit = chk_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_prepare p; ev_prepare_init(&p, prep_cb); ev_prepare_start(l, &p);
  ev_check c; ev_check_init(&c, chk_cb); ev_check_start(l, &c);
  ev_timer t; ev_timer_init(&t, timer_cb, 0.01, 0.0); ev_timer_start(l, &t); /* give the loop work */
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(prep_hit, "prepare");
  OK(chk_hit, "check");
  ev_loop_destroy(l);
}
#else
static void test_prepare_check(void) { SKIP("prepare/check", "EV_PREPARE_ENABLE is 0 in this build"); }
#endif


/* ---- async ---- */
#if EV_ASYNC_ENABLE
static int async_hit;
static void async_cb(struct ev_loop *l, ev_async *w, int r) { (void)r; async_hit = 1; ev_async_stop(l, w); ev_break(l, EVBREAK_ALL); }
static void test_async(void) {
  async_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_async a; ev_async_init(&a, async_cb); ev_async_start(l, &a);
  ev_async_send(l, &a);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(async_hit, "async");
  ev_loop_destroy(l);
}
#else
static void test_async(void) { SKIP("async", "EV_ASYNC_ENABLE is 0 in this build"); }
#endif


/* ---- fork ---- */
#if EV_FORK_ENABLE
static int fork_hit;
static void fork_cb(struct ev_loop *l, ev_fork *w, int r) { (void)r; fork_hit = 1; ev_fork_stop(l, w); ev_break(l, EVBREAK_ALL); }
static void test_fork(void) {
  fork_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_fork fw; ev_fork_init(&fw, fork_cb); ev_fork_start(l, &fw);
  ev_loop_fork(l); /* mark a fork; fork watchers fire on the next iteration */
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(fork_hit, "fork");
  ev_loop_destroy(l);
}
#else
static void test_fork(void) { SKIP("fork", "EV_FORK_ENABLE is 0 in this build"); }
#endif


/* ---- cleanup ---- */
static int cleanup_hit;
static void cleanup_cb(struct ev_loop *l, ev_cleanup *w, int r) { (void)l; (void)w; (void)r; cleanup_hit = 1; }
static void test_cleanup(void) {
  cleanup_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_cleanup c; ev_cleanup_init(&c, cleanup_cb); ev_cleanup_start(l, &c);
  ev_loop_destroy(l); /* cleanup watchers fire on loop teardown */
  OK(cleanup_hit, "cleanup");
}

/* ---- ev_once ---- */
static int once_hit;
static void once_cb(int revents, void *arg) { (void)revents; *(int *)arg = 1; }
static void test_once(void) {
  once_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_once(l, -1, 0, 0.01, once_cb, &once_hit);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(once_hit, "ev_once");
  ev_loop_destroy(l);
}

#ifndef _WIN32
/* ---- io ---- */
static int io_hit;
static void io_cb(struct ev_loop *l, ev_io *w, int r) {
  (void)r; char b[8]; ssize_t n = read(w->fd, b, sizeof b); (void)n;
  io_hit = 1; ev_io_stop(l, w); ev_break(l, EVBREAK_ALL);
}
static void test_io(void) {
  io_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { SKIP("io", "socketpair"); ev_loop_destroy(l); return; }
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  ev_io io; ev_io_init(&io, io_cb, sv[0], EV_READ); ev_io_start(l, &io);
  char c = 'x'; ssize_t n = write(sv[1], &c, 1); (void)n;
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(io_hit, "io");
  close(sv[0]); close(sv[1]); ev_loop_destroy(l);
}

/* ---- signal ---- */
static int sig_hit;
static void sig_cb(struct ev_loop *l, ev_signal *w, int r) { (void)r; sig_hit = 1; ev_signal_stop(l, w); ev_break(l, EVBREAK_ALL); }
static void test_signal(void) {
  sig_hit = 0;
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_signal sw; ev_signal_init(&sw, sig_cb, SIGUSR1); ev_signal_start(l, &sw);
  raise(SIGUSR1);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 2.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(sig_hit, "signal");
  ev_loop_destroy(l);
}

/* ---- child ---- */
#if EV_CHILD_ENABLE
static int child_hit;
static void child_cb(struct ev_loop *l, ev_child *w, int r) { (void)r; child_hit = 1; ev_child_stop(l, w); ev_break(l, EVBREAK_ALL); }
static void test_child(void) {
  child_hit = 0;
  struct ev_loop *l = ev_default_loop(0); /* ev_child requires the default loop */
  pid_t pid = fork();
  if (pid == 0) { _exit(0); }
  if (pid < 0) { SKIP("child", "fork failed"); return; }
  ev_child c; ev_child_init(&c, child_cb, pid, 0); ev_child_start(l, &c);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 3.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  ev_timer_stop(l, &wd);
  OK(child_hit, "child");
}
#else
static void test_child(void) { SKIP("child", "EV_CHILD_ENABLE is 0 in this build"); }
#endif


/* ---- stat ---- */
static int stat_hit;
static void stat_cb(struct ev_loop *l, ev_stat *w, int r) { (void)r; stat_hit = 1; ev_stat_stop(l, w); ev_break(l, EVBREAK_ALL); }
static char stat_path[256];
static void stat_touch(struct ev_loop *l, ev_timer *w, int r) {
  (void)l; (void)w; (void)r;
  FILE *f = fopen(stat_path, "a"); if (f) { fputs("x", f); fclose(f); }
}
static void test_stat(void) {
  stat_hit = 0;
  snprintf(stat_path, sizeof stat_path, "%s/qbev_stat_%d.tmp", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  FILE *f = fopen(stat_path, "w"); if (f) { fputs("init", f); fclose(f); }
  struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
  ev_stat s; ev_stat_init(&s, stat_cb, stat_path, 0.2); ev_stat_start(l, &s);
  ev_timer touch; ev_timer_init(&touch, stat_touch, 0.05, 0.0); ev_timer_start(l, &touch);
  ev_timer wd; ev_timer_init(&wd, wd_cb, 5.0, 0.0); ev_timer_start(l, &wd);
  ev_run(l, 0);
  OK(stat_hit, "stat");
  remove(stat_path); ev_loop_destroy(l);
}

/* ---- embed (needs an embeddable inner backend + real I/O) ---- */
#if EV_EMBED_ENABLE
static int embed_hit;
static void embed_io_cb(struct ev_loop *l, ev_io *w, int r) {
  (void)l; (void)r; char b[8]; ssize_t n = read(w->fd, b, sizeof b); (void)n; embed_hit = 1;
}
static void embed_done(struct ev_loop *l, ev_timer *w, int r) { (void)w; (void)r; ev_break(l, EVBREAK_ALL); }
static void test_embed(void) {
  embed_hit = 0;
  struct ev_loop *outer = ev_loop_new(EVFLAG_AUTO);
  struct ev_loop *inner = ev_loop_new(EVFLAG_NOENV | ev_recommended_backends());
  if (!inner || !(ev_embeddable_backends() & ev_backend(inner))) {
    SKIP("embed", "inner backend not embeddable");
    if (inner) ev_loop_destroy(inner);
    ev_loop_destroy(outer);
    return;
  }
  int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { SKIP("embed", "socketpair"); ev_loop_destroy(inner); ev_loop_destroy(outer); return; }
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  ev_io io; ev_io_init(&io, embed_io_cb, sv[0], EV_READ); ev_io_start(inner, &io);
  ev_embed emb; ev_embed_init(&emb, 0, inner); ev_embed_start(outer, &emb); /* cb=0 → auto-sweep */
  char c = 'x'; ssize_t n = write(sv[1], &c, 1); (void)n;
  ev_timer wd; ev_timer_init(&wd, embed_done, 2.0, 0.0); ev_timer_start(outer, &wd);
  ev_run(outer, 0);
  OK(embed_hit, "embed");
  close(sv[0]); close(sv[1]); ev_loop_destroy(inner); ev_loop_destroy(outer);
}
#else
static void test_embed(void) { SKIP("embed", "EV_EMBED_ENABLE is 0 in this build"); }
#endif

#endif /* !_WIN32 */

int main(void) {
  printf("qb-ev watcher coverage (ev %d.%d, backends=0x%x)\n",
         ev_version_major(), ev_version_minor(), ev_supported_backends());

  test_timer();
  test_periodic();
  test_idle();
  test_prepare_check();
  test_async();
  test_fork();
  test_cleanup();
  test_once();
#ifndef _WIN32
  test_io();
  test_signal();
  test_child();
  test_stat();
  test_embed();
#else
  SKIP("io/signal/child/stat/embed", "POSIX-only in this test");
#endif

  printf("\n== watchers: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);

  /* A suite that skipped everything exits 0 and reads as a pass, which is how a build with the
     watcher families compiled out would have reported itself once the families became
     conditional above. Four checks are unconditional in EVERY combination -- timer, periodic,
     cleanup and once are in no reduced profile and need nothing POSIX-only -- so four is the
     floor. It is deliberately the minimum over all of them rather than the count this platform
     happens to reach: measured, a full POSIX build runs 14, a reduced POSIX build 7, and a
     reduced Windows build exactly these 4. */
  if (g_run < 4) {
    printf("== FAIL: only %d checks ran; timer, periodic, cleanup and once are unconditional ==\n",
           g_run);
    return 1;
  }
  return g_fail ? 1 : 0;
}
