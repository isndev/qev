# Changelog

All notable changes to **qev** are documented in this file. The format is based
on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- **The reduced watcher profile keeps `async`.** `QB_EV_WATCHERS_FULL=OFF` used to compile out
  seven families; it now compiles out six — idle, prepare, check, fork, child, embed — and
  leaves `ev_async_start/stop/send` in. An embedder that parks a thread inside `ev_run` needs
  one entry point another thread may call against that concurrent `ev_run`, and `ev_async_send`
  is the only one libev offers; qb 3.2's `VirtualCore` is that embedder. Cost, measured on
  Linux/x86-64: three exported symbols and 24 bytes of `struct ev_loop` (61 `ev_*` / 496 bytes
  against the full profile's 74 / 592).

### Fixed

- **A loop with no fd watcher no longer pays a backend poll it cannot use.** `ev_run` called
  `backend_poll` unconditionally, so an `EVRUN_NOWAIT` pass over a loop holding only timers —
  the shape an embedder's request timeout, retry or sleep leaves behind — cost a bare
  `epoll_wait(0)` / `kevent` / wepoll syscall on every pass for the life of the timer. Measured
  in qb 3.2's core on i9-12900K / WSL2 g++-14: a coroutine request/reply that arms a 500 ms
  timeout cost **~800 ns** per round trip against 46 ns without the timeout, three passes each
  paying the syscall. The loop now keeps a count of its active `ev_io` watchers (the evpipe
  behind signals and async watchers and the timerfd included, since they are `ev_io`s) and
  skips the poll when that count is zero AND the wait would not block anyway; a blocking wait
  is kept, because with no fd it is the sleep. Timers, periodics, idle/prepare/check and
  pending events are reified exactly as before — `tests/test-loops.c` (`test_io_count`) pins
  that a timers-only `EVRUN_NOWAIT` pass still fires an expired timer and that a timers-only
  blocking run still sleeps.

### Added

- **`ev_io_count(loop)`** (`EV_FEATURE_API`, plus `loop_ref::io_count()` in `ev++.h`): the
  number of active `ev_io` watchers — the count the fix above reads. Covered by
  `tests/test-loops.c` (`test_io_count`, four unconditional checks plus four on POSIX; the
  suite's unconditional floor rises 16 → 20). The exported census is 78 `ev_*` on POSIX and 77
  on Windows.
- **`ev_active_count_addr(loop)` / `ev_pending_count_addr(loop)` and `EV_NUMPRI`** (`EV_FEATURE_API`,
  plus `loop_ref::active_count_addr()` / `pending_count_addr()` in `ev++.h`): read-only aliases of
  the two counters `ev_active_count` and `ev_pending_count` report, pointing into the loop and
  valid until `ev_loop_destroy`. They exist for the same embedder as `ev_active_count`: a scheduler
  that gates every `EVRUN_NOWAIT` pass on "is there anything to do" was paying two out-of-line
  calls and a loop over the priorities on every pass — measured at ~10 % of a one-event pass in
  qb 3.2's core once nothing else in that pass read a clock — where six inline loads answer the
  same question. `*ev_active_count_addr` is the raw referenced-active count (it reads −1 between
  an `ev_unref` and the start it pairs with; test it with `> 0`), and `ev_pending_count_addr` has
  `EV_NUMPRI` entries — one per priority level, the constant is now public — whose sum is
  `ev_pending_count`. Owner thread only, like any other read of the loop. Covered by
  `tests/test-loops.c` (`test_count_addr`, six checks; the suite's unconditional floor rises
  10 → 16 with them).
- **`ev_active_count(loop)`** (`EV_FEATURE_API`, plus `loop_ref::active_count()` and
  `loop_ref::pending_count()` in `ev++.h`): the number of *referenced* active watchers — the
  quantity `ev_run` itself consults to decide whether it keeps looping. It exists for embedders
  that drive the loop with `EVRUN_NOWAIT` from their own scheduler and need to know, before
  paying for a backend poll and two clock reads, whether the loop has anything at all to do:
  `ev_run(EVRUN_NOWAIT)` polls unconditionally, even over an empty loop, and a hot loop calling
  it once per pass measures that cost on every pass. Watchers that were `ev_unref`'d are not
  counted, by design (they do not keep the loop alive either); pending events are reported
  separately by `ev_pending_count`. Covered by `tests/test-loops.c`.
- **A wepoll test suite** (`tests/test-wepoll.c`, Windows-only by registration): the fork's
  headline feature had no dedicated test. Five cases over native winsock SOCKETs, including
  the regression test for the interest-set-modification bug that motivated the fork — a live
  watcher widened to `READ|WRITE` whose re-registration never reached the backend.
- **A loop-mechanics suite** (`tests/test-loops.c`, every platform): multi-loop isolation,
  timer ordering and pacing, `ev_timer_again`, priority-ordered invocation, the README's
  "NULL if unavailable" backend promise, `ev_now`/`ev_iteration` bookkeeping, `ev_feed_event`,
  a real `fork()` + `ev_loop_fork`, a cross-thread `ev_async_send`, and a thousand concurrent
  timers. Both suites carry the same anti-vacuity floor as the watcher suite: a run below its
  unconditional check count fails instead of reading as a pass.

## [5.0.0] — 2026-08-19

qev 5.0 is the first release of the maintained continuation of **libev 4.33**, which
upstream stopped at in 2020. It bundles a hardened **wepoll 1.5.8** for native Windows
support and carries around **70 substantive code changes** versus 4.33 (license-header
swaps, version bumps and whitespace excluded).

### The C API is libev's, unchanged

`ev_run`, `ev_io_start`, `ev_timer_init`, `struct ev_loop` — every name, every
signature, every semantic. Porting a libev program is one include-path edit:
`<ev.h>` becomes `<qev/ev.h>`. Nothing else moves.

### Everything that reaches a filesystem is ours

An installed qev never overwrites an installed libev. The archive is `libqev.a`, the
headers install under `include/qev/`, the CMake package is `find_package(qev)` →
`qb::ev`, pkg-config is `qev.pc`, the manual page is `qev.3`, and the include guards
are `QB_EV_*` so one translation unit can hold both `<qev/ev.h>` and a real `<ev.h>`.

What is **not** supported is linking qev and a real libev into the same program: both
export the same `ev_*` symbols and libev is a single translation unit, so an archive
is all-or-nothing. The linker either refuses with `duplicate symbol`, or — when the
program touches only the symbols both provide — resolves them silently from whichever
archive came first.

qev is **not** binary-compatible with libev 4.x and does not claim to be:
`ev_version_major()` reports 5, and libev's own documented check is
`assert(ev_version_major() == EV_VERSION_MAJOR)`, which a 4.x consumer fails by
construction. Recompile; do not swap the object.

The `event.h` **libevent** compatibility layer is retained but **no longer built by
default** (`-DQB_EV_LIBEVENT_COMPAT=ON`). It exports 24 *unprefixed* upstream libevent
symbols, and libevent is far more widely deployed than libev.

### Watcher families

All fourteen are built in a standalone qev. A host that sets `QB_EV_WATCHERS_FULL=OFF`
— as the qb Actor Framework does — compiles out idle, prepare, check, fork, child,
async and embed: 16 fewer symbols on that host's link line, and a smaller
`struct ev_loop`.

### Packaging

- **Component-aware install.** `qev_Runtime` carries the versioned shared object and
  its SONAME symlink; `qev_Development` carries headers, static archive, unversioned
  link symlink, CMake package and `qev.pc` — the `libqev` / `libqev-dev` split, drawn
  by the build instead of by hand.
- **CPack**, binary (per component) and source.
- **`DEBUG_POSTFIX`** so a Debug and a Release build share a prefix without the second
  overwriting the first.
- **`target_compile_features(qev PUBLIC c_std_99)`** — the language level is a
  contract, not a hope.
- **`BUILD_TESTING`** honoured, so the switch every CI already sets works here.
- **`LICENSE` and `THIRD-PARTY-NOTICES` are installed**, into both components. BSD-2
  clause 2 asks a binary redistribution to carry the upstream notice, and until this
  release nothing put either file on disk — only the source archive was compliant,
  and only because CPack copies the whole tree into it.
- **The manual page is generated by the CMake build too**, when `pod2man` is present.
  `qev.3` had been an autotools-only artefact while README and CHANGELOG both named it
  among the things an install produces.
- **`autogen.sh` is executable.** It was committed mode 644, so the `./autogen.sh` both
  README and CONTRIBUTING tell you to run answered `permission denied`.
- **The reduced watcher profile builds its own tests.** `-DQB_EV_WATCHERS_FULL=OFF` — the
  profile qb ships — did not compile the watcher suite, so the verification CONTRIBUTING
  asks for could not be performed. Each family is now gated on its `EV_*_ENABLE` macro and
  skips rather than failing to build, and the suite refuses to report success when fewer
  than its four unconditional checks ran.

The two largest pieces of work are a from-scratch rewrite of the `io_uring`
backend and making libev's `epoll` backend genuinely usable on Windows via IOCP.

### Backends — new & rewritten

- **`io_uring` backend rewritten from scratch** (`ev_iouring.c`): uses the kernel
  `<linux/io_uring.h>` ABI, precomputed ring pointers instead of offset
  arithmetic, and a clean submit/drain/poll structure. Validates the
  kernel-reported ring layout (rejects zero entry counts, overflowing/oversized
  maps, out-of-window head/tail/mask/array offsets), flushes the CQ-overflow
  backlog, falls back when `MAP_POPULATE` is refused, bounds its drain/spin/EINTR
  budgets, and cleans up every resource on each init-failure path. Opts into
  `IORING_SETUP_SINGLE_ISSUER`/`COOP_TASKRUN` with graceful downgrade on old
  kernels; a documented decision *not* to use multishot `POLL_ADD` (level-triggered,
  breaks libev's one-shot semantics).
- **Windows `epoll` backend via vendored wepoll** (`ev_epoll.c`, `wepoll.c/.h`):
  on `_WIN32` the backend uses wepoll (IOCP/AFD-backed epoll) with a real
  `fd → SOCKET` translation, so `ev_io` can finally watch native winsock handles —
  something upstream's `_osfhandle` aliasing never actually made work.
- **`event_compat.h`** vendored so legacy libevent 1.x code compiles against qev.

### Added

- **Native-socket `ev_io` on Win32**: an `ev_io.handle` member, `ev_io_set_sock`/
  `ev_io_init_sock` and `ev_win32_socket_fd()`, backed by a thread-safe, refcounted
  `SOCKET ↔ fd` registry (`CRITICAL_SECTION` + `INIT_ONCE`); mirrored by C++
  `ev::io` overloads in `ev++.h`.
- **Compile-time ABI assertions** (`EV_STATIC_ASSERT`) pinning the watcher
  common-initial-sequence, so a future field reorder fails the build instead of
  becoming silent UB in the up-casts.
- **Standalone tooling**: a watcher-coverage test (`tests/test-watchers.c`,
  exercises all 14 watcher types), a cross-backend benchmark
  (`bench/bench-backends.c`, reports the best backend on the machine), a build
  matrix script (`scripts/test-build-matrix.sh`), GitHub Actions CI, modern CMake
  package config (`find_package(libev)`), and updated autotools.

### Changed

- **macOS uses `kqueue` instead of `select`**: upstream strips `KQUEUE` on Apple
  and forces `select` (O(n), `FD_SETSIZE` 1024 ceiling); qev keeps kqueue for
  socket/pipe loops (only stripping the kqueue-backed, unreliable macOS `poll`),
  with a documented caveat that kqueue is not used for regular-file readiness.
- **`select` demoted to a strict last-resort fallback** on every platform.
- **`backend_fd` widened to `uintptr_t`** across every backend, with a uniform
  `EV_BACKEND_FD_INVALID` sentinel — enables Windows `HANDLE` storage and avoids
  sign issues.
- **Consistent `EPOLLRDHUP`/`POLLRDHUP` TCP half-close** across epoll, poll, port
  and wepoll, so a peer shutdown wakes the read watcher.
- **`MAX_BLOCKTIME` lowered 59.743 → 0.743 s**, with the longer `MAX_BLOCKTIME2`
  sleep gated on a monotonic clock.

### Fixed

- **`ev_loop_destroy` use-after-close**: upstream unconditionally `close()`'d
  `backend_fd` *before* `iouring_destroy`/`linuxaio_destroy`, which then
  munmap'd/stopped a watcher on a dead fd. qev skips the close for those backends
  and adds a wepoll `epoll_close` path.
- **kqueue dropped registrations on EINTR**: the changelist was cleared before
  `kevent` consumed it; now `kevent` is EINTR-retried and the changelist cleared
  only after success.
- **win32 `accept()` failure detection**: compared against `INVALID_SOCKET`
  (Winsock never returns negative) instead of `< 0` — upstream's check never fired
  (flagged with a TODO upstream).
- **`port` backend scanned uninitialized completion slots**: `port_getn` leaves
  `nget` undefined on error; qev forces `nget = 0`.
- **`linuxaio` epoll `backend_fd` leak** closed on `io_setup` failure; the internal
  epoll watcher is stopped in destroy; `io_events[0]` zero-length array → C99 FAM.
- **io_uring syscall-name typo**: upstream defined the fallback under the
  non-existent `SYS_io_uring_wregister`; qev uses the real `SYS_io_uring_register`
  (427).
- **libevent compat shim (`event.c`)**: NULL-pointer guards across the whole API,
  idempotent `event_init`, correct `event_base_free`, and `event_pending` reports
  the true remaining timeout (via `ev_timer_remaining`) honoring the events mask.
- **`ev_once`** validates its arguments and no longer leaks when the callback could
  never fire; **`ev_realloc_emul`** handles negative/zero sizes correctly.
- **`assert(("msg", expr))` comma-operator idiom** replaced with
  `EV_ASSERT_MSG(expr, msg)` (the string, not the condition, was being asserted).

### Hardening (undefined behaviour / robustness)

- **Array-growth integer overflow**: geometric doubling now runs in `size_t`
  (signed-int `<<=` overflow is UB), the narrowing is clamped to `INT_MAX`, and
  `array_realloc` rejects byte sizes over `LONG_MAX` via `ev_syserr`.
- **inotify read buffer aligned** to `struct inotify_event` via a union (was
  misaligned int loads — UB, faults on strict-alignment CPUs).
- **fd-range guards** before indexing `anfds[]` in the epoll/kqueue/poll/port paths.
- **Bounded EINTR retry loops** (cap 255) around `select()`, `kevent()` and the
  linuxaio `io_cancel` resubmit (was an unbounded `for(;;)`).
- **`offsetof` inline accessors** replace strict-aliasing watcher casts (paired with
  the compile-time ABI assertions above).

### Portability

- **Config-macro guards rewritten** from bare token tests to `defined()` checks
  (dozens of `__linux`/`__GLIBC__`/`_MSC_VER`/`SYS_*` conditions) — no `-Wundef`
  breakage under strict builds.
- **Time-conversion macros** (`EV_TV_SET`/`EV_TS_SET`/`…_GET`) made overflow- and
  type-safe (cast through `ev_tstamp`/`time_t`, platform-correct `tv_usec` type).
- **Dual config-header discovery** (CMake `ev_config.h` vs autotools `config.h`);
  `ev.h` is self-contained via `__has_include`.
- **`fd_valid` on Win32 uses `getsockopt(SO_ERROR)`** (mapping `WSAENOTSOCK` →
  `EBADF`) instead of comparing a SOCKET to `-1`.

### Build & modernization

- **Explicit parentheses in `ev_io_modify`**: `(ev)->events & EV__IOFDSET | (events_)`
  → `((ev)->events & EV__IOFDSET) | (events_)`. Behaviour is unchanged (`&` already
  binds tighter than `|`); this silences `-Wparentheses` under strict builds.
- **Warning-clean** under a strict `-Wall -Wextra -Wpedantic -Wshadow -Wcast-align …`
  set; passes ASan/UBSan; validated on Linux (GCC/Clang) and macOS.
- **MIT relicensing** (original libev/wepoll BSD-2 notices retained), SPDX headers,
  standalone CMake with `find_package(libev)`, pkg-config (`libev.pc`), version
  bumped to 5.0.

### wepoll fixes (on top of wepoll 1.5.8)

- **`epoll__create`** closes the IOCP handle before `port_delete` on the
  handle-collision path (upstream leaked it).
- **`epoll_ctl`/`epoll_wait`** reject NULL `event`/`events` buffers with `EINVAL`
  instead of dereferencing NULL.
- **`epoll_ctl` EBADF path** validates the SOCKET via `getsockopt(SO_ERROR)` rather
  than misapplying `GetHandleInformation` to a winsock SOCKET.
- **`port_wait`** guards `maxevents * sizeof` against overflow and returns `ENOMEM`
  on allocation failure instead of silently truncating to a 256-entry buffer.

---

Based on libev © Marc Alexander Lehmann and wepoll © Bert Belder; see
[THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

qev forks libev at 4.33. For the pre-5.0 (upstream libev) history, see the
[libev changelog](http://cvs.schmorp.de/libev/Changes?view=markup).

[5.0.0]: https://github.com/isndev/qev/releases/tag/v5.0.0
