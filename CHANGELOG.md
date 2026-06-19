# Changelog

All notable changes to **qb-ev** are documented in this file. The format is based
on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to
[Semantic Versioning](https://semver.org/).

## [5.0.0] — 2026-06-19

qb-ev 5.0 is a heavily modernized fork of **libev 4.33** (upstream is effectively
unmaintained) that also bundles a hardened **wepoll 1.5.8** for native Windows
support. The `ev_*` API/ABI is preserved — existing libev code relinks unchanged;
only `ev_version_major()` now reports `5`. Around **70 substantive code changes**
versus upstream (license-header swaps, version bumps and whitespace excluded).

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
- **`event_compat.h`** vendored so legacy libevent 1.x code compiles against qb-ev.

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
  and forces `select` (O(n), `FD_SETSIZE` 1024 ceiling); qb-ev keeps kqueue for
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
  munmap'd/stopped a watcher on a dead fd. qb-ev skips the close for those backends
  and adds a wepoll `epoll_close` path.
- **kqueue dropped registrations on EINTR**: the changelist was cleared before
  `kevent` consumed it; now `kevent` is EINTR-retried and the changelist cleared
  only after success.
- **win32 `accept()` failure detection**: compared against `INVALID_SOCKET`
  (Winsock never returns negative) instead of `< 0` — upstream's check never fired
  (flagged with a TODO upstream).
- **`port` backend scanned uninitialized completion slots**: `port_getn` leaves
  `nget` undefined on error; qb-ev forces `nget = 0`.
- **`linuxaio` epoll `backend_fd` leak** closed on `io_setup` failure; the internal
  epoll watcher is stopped in destroy; `io_events[0]` zero-length array → C99 FAM.
- **io_uring syscall-name typo**: upstream defined the fallback under the
  non-existent `SYS_io_uring_wregister`; qb-ev uses the real `SYS_io_uring_register`
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
- **inotify read buffer aligned** to `struct ev_inotify_event` via a union (was
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

qb-ev forks libev at 4.33. For the pre-5.0 (upstream libev) history, see the
[libev changelog](http://cvs.schmorp.de/libev/Changes?view=markup).

[5.0.0]: https://github.com/isndev/qb-ev/releases/tag/v5.0
