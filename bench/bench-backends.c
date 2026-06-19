/*
 * qb-ev — cross-backend stress benchmark (standalone, no external deps).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor).
 *
 * Sweeps every backend libev was built with and runs two workloads on each, then
 * prints the winner — i.e. which backend to prefer on THIS machine.
 *
 *   dispatch  : N socketpairs, ALL active each round  -> raw dispatch throughput
 *   active-few: N registered, only K=16 active        -> O(N) vs O(active) test
 *               (select/poll scan all N per wait; epoll/kqueue/io_uring/port
 *                report only the ready ones — the gap is why they exist)
 */
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <fcntl.h>
# include <sys/resource.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <unistd.h>

static long g_events;

static void on_readable(struct ev_loop *l, ev_io *w, int r) {
  (void)l; (void)r; char b[256]; while (read(w->fd, b, sizeof b) > 0) {} ++g_events;
}
static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

static const char *bname(unsigned b) {
  switch (b) {
    case EVBACKEND_SELECT:   return "select";
    case EVBACKEND_POLL:     return "poll";
    case EVBACKEND_EPOLL:    return "epoll";
    case EVBACKEND_KQUEUE:   return "kqueue";
    case EVBACKEND_PORT:     return "port";
    case EVBACKEND_LINUXAIO: return "linuxaio";
    case EVBACKEND_IOURING:  return "iouring";
    default:                 return "other";
  }
}

/* Returns events/sec, -1 if the backend is unavailable, -2 if skipped (FD cap). */
static double run_bench(unsigned backend, int N, int active_k, double seconds) {
  if (backend == EVBACKEND_SELECT && N >= FD_SETSIZE - 16) return -2;
  struct ev_loop *l = ev_loop_new(backend);
  if (!l || ev_backend(l) != backend) { if (l) ev_loop_destroy(l); return -1; }

  int (*sv)[2] = calloc((size_t)N, sizeof *sv);
  ev_io *w = calloc((size_t)N, sizeof *w);
  int made = 0;
  for (int i = 0; i < N; i++) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) != 0) break;
    set_nonblock(sv[i][0]);
    ev_io_init(&w[i], on_readable, sv[i][0], EV_READ);
    ev_io_start(l, &w[i]);
    made = i + 1;
  }
  N = made;
  double eps = -1;
  if (N > 0) {
    ev_run(l, EVRUN_NOWAIT); /* warm-up: flush the registration changelist */
    int K = active_k < N ? active_k : N;
    long total = 0; size_t cursor = 0;
    double t0 = ev_time(), t1 = t0;
    do {
      g_events = 0;
      for (int j = 0; j < K; j++) {
        size_t idx = (cursor + (size_t)j) % (size_t)N;
        char b = 'x'; ssize_t n = write(sv[idx][1], &b, 1); (void)n;
      }
      cursor = (cursor + (size_t)K) % (size_t)N;
      long guard = 0;
      while (g_events < K && ++guard < 4000000L) ev_run(l, EVRUN_NOWAIT);
      total += g_events;
      t1 = ev_time();
    } while (t1 - t0 < seconds);
    eps = total / (t1 - t0);
  }
  for (int i = 0; i < N; i++) { ev_io_stop(l, &w[i]); close(sv[i][0]); close(sv[i][1]); }
  free(sv); free(w); ev_loop_destroy(l);
  return eps;
}
#endif /* !_WIN32 */

int main(void) {
#ifndef _WIN32
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }

  const unsigned sup = ev_supported_backends();
  printf("qb-ev backend benchmark — ev %d.%d, supported=0x%x\n\n",
         ev_version_major(), ev_version_minor(), sup);
  printf("%-10s %-11s %8s %16s\n", "backend", "workload", "fds", "events/sec");
  printf("-------------------------------------------------------\n");

  const unsigned cands[] = {
    EVBACKEND_SELECT, EVBACKEND_POLL, EVBACKEND_EPOLL,
    EVBACKEND_KQUEUE, EVBACKEND_PORT, EVBACKEND_LINUXAIO, EVBACKEND_IOURING,
  };
  const int scales[] = { 64, 512, 4096, 16384 };

  double best_eps = 0; const char *best = "(none)"; int best_n = 0;

  for (size_t bi = 0; bi < sizeof cands / sizeof *cands; ++bi) {
    unsigned b = cands[bi];
    if (!(sup & b)) continue;
    const int scalable = (b != EVBACKEND_SELECT && b != EVBACKEND_POLL);
    for (size_t si = 0; si < sizeof scales / sizeof *scales; ++si) {
      int n = scales[si];
      if (!scalable && n > 4096) continue; /* avoid poll() huge-nfds abort + select wall */

      double d = run_bench(b, n, n, 0.15);
      if (d >= 0) printf("%-10s %-11s %8d %16.0f\n", bname(b), "dispatch", n, d);

      double a = run_bench(b, n, 16, 0.15);
      if (a >= 0) {
        printf("%-10s %-11s %8d %16.0f\n", bname(b), "active-few", n, a);
        /* "Best" = the backend that scales furthest, then fastest there — that is
         * the regime where the backend choice actually matters (at tiny N every
         * backend is fast). */
        if (n > best_n || (n == best_n && a > best_eps)) { best_eps = a; best = bname(b); best_n = n; }
      }
    }
  }

  printf("\n==> Best backend on this machine (active-few @ %d fds): %s  (%.0f events/sec)\n",
         best_n, best, best_eps);
  return 0;
#else
  printf("qb-ev benchmark is POSIX-only (uses socketpair); not built on Windows.\n");
  return 0;
#endif
}
