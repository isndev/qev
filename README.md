# qb-ev

[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![version](https://img.shields.io/badge/version-5.0-brightgreen.svg)](CHANGELOG.md)
![C11](https://img.shields.io/badge/C-11-00599C.svg)
![platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20BSD%20%7C%20illumos-lightgrey.svg)
![build](https://img.shields.io/badge/build-CMake%20%7C%20autotools-orange.svg)

**A modernized, truly cross-platform fork of [libev](http://software.schmorp.de/pkg/libev.html).**

`qb-ev` is a hardened, drop-in fork of Marc Lehmann's libev. It keeps the exact
same `ev_*` C API and ABI (and the `libevent` compatibility shim), but fixes,
modernizes and tunes every backend so that the *best* I/O multiplexer is used on
each platform — including a real `epoll` on Windows (via [wepoll](https://github.com/piscisaureus/wepoll))
instead of `select`, and `kqueue` on macOS instead of `select`.

It is maintained as the event-loop core of the [qb Actor Framework](https://github.com/isndev/qb),
and is released standalone under the MIT license.

> Version **5.0** — picks up where upstream libev (4.x, unmaintained) left off.

---

## Table of contents

- [Why a fork?](#why-a-fork)
- [Highlights](#highlights)
- [Backends per platform](#backends-per-platform)
- [Quick start](#quick-start)
- [Choosing a backend at runtime](#choosing-a-backend-at-runtime)
- [What changed vs. upstream libev](#what-changed-vs-upstream-libev)
- [Building & testing standalone](#building--testing-standalone)
- [Threading model](#threading-model)
- [Performance](#performance)
- [Compatibility & ABI](#compatibility--abi)
- [FAQ](#faq)
- [Credits](#credits)
- [License](#license)

---

## Why a fork?

libev is excellent and battle-tested, but upstream is effectively unmaintained
(last release 4.33/4.35), some platform paths had aged badly, and a few latent
bugs sat behind rarely-exercised code paths:

- On **macOS** the loop defaulted to `select` — capped at `FD_SETSIZE` (1024) and
  `O(N)` per poll. `qb-ev` defaults to **`kqueue`**.
- On **Windows** libev only ever offered `select`. `qb-ev` ships a hardened,
  integrated **`epoll` (wepoll / IOCP)** backend with a correct `fd ↔ SOCKET`
  mapping and TCP half-close (`EPOLLRDHUP`).
- The **`io_uring`** backend was rewritten for correctness (ring-layout
  validation, memory fences, stale-CQE filtering, CQ-overflow recovery).
- Various **portability / UB / robustness** fixes throughout (see below), and the
  whole tree now builds **warning-clean** under a strict GCC/Clang flag set and
  passes ASan/UBSan.

The public contract is unchanged: if your code builds against libev, it builds
against `qb-ev`.

## Highlights

- **Best backend by default, everywhere.** Selection always prefers the most
  scalable mechanism and falls back to `select` only as a last resort.
- **Cross-platform for real:** Linux (`epoll`, optional `io_uring` / `linuxaio`),
  macOS / *BSD (`kqueue`), Solaris/illumos (event ports), Windows (`epoll` via
  wepoll), plus portable `poll` / `select`.
- **Drop-in:** same `ev_*` API/ABI and the `event.h` libevent compatibility shim.
- **Hardened:** signed-overflow, alignment, fd-range, EINTR and double-close
  fixes; strict-aliasing-safe accessors; a compile-time ABI contract.
- **Clean & modern:** SPDX headers, MIT license, standalone CMake with
  `find_package(libev)` support, autotools kept and updated.

## Backends per platform

| Platform            | Default (auto) | Also available                         | Last resort |
|---------------------|----------------|----------------------------------------|-------------|
| Linux               | `epoll`        | `io_uring`, `linuxaio` (opt-in), `poll`| `select`    |
| macOS / FreeBSD / *BSD | `kqueue`    | `poll`                                 | `select`    |
| Solaris / illumos   | event `port`   | `poll`                                 | `select`    |
| Windows             | `epoll` (wepoll/IOCP) | —                               | `select`    |

`io_uring` and `linuxaio` are compiled when the kernel headers are present but are
**not** auto-selected (readiness-style loops do not benefit from them in practice);
opt in explicitly with `EVBACKEND_IOURING` / `EVBACKEND_LINUXAIO` or by building
with `-DEV_RECOMMEND_IOURING=1`.

## Quick start

### CMake — `add_subdirectory`

```cmake
add_subdirectory(qb-ev)        # provides target `ev`
target_link_libraries(myapp PRIVATE qb::ev)   # or plain `ev`, or `ev::ev`
```

### CMake — `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(qb-ev
  GIT_REPOSITORY https://github.com/isndev/qb-ev.git
  GIT_TAG        v5.0)
FetchContent_MakeAvailable(qb-ev)
target_link_libraries(myapp PRIVATE qb::ev)
```

### CMake — installed package

```cmake
find_package(libev 5 REQUIRED)
target_link_libraries(myapp PRIVATE qb::ev)   # `ev::ev` alias also provided
```

### C usage

```c
#include <ev.h>
#include <stdio.h>

static void timer_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    puts("tick");
    ev_break(loop, EVBREAK_ONE);
}

int main(void) {
    struct ev_loop *loop = ev_default_loop(0);   /* picks the best backend */
    ev_timer t;
    ev_timer_init(&t, timer_cb, 1.0, 0.0);
    ev_timer_start(loop, &t);
    ev_run(loop, 0);
    return 0;
}
```

### C++ usage

```cpp
#include <ev++.h>

ev::timer t;
t.set<my_class, &my_class::on_timeout>(this);
t.start(1.0, 0.0);
ev::get_default_loop().run();
```

## Choosing a backend at runtime

```c
/* Force a specific backend (returns NULL if unavailable on this build/host). */
struct ev_loop *loop = ev_loop_new(EVBACKEND_KQUEUE);

/* What did we actually get, and what is supported? */
unsigned be        = ev_backend(loop);            /* EVBACKEND_* */
unsigned supported = ev_supported_backends();
unsigned best      = ev_recommended_backends();
```

The recommended order is best-first per platform, with `select` always last.

## What changed vs. upstream libev

qb-ev is **not** a cosmetic fork. It carries **~70 substantive changes** over libev
4.33 (and bundles a hardened wepoll 1.5.8), while keeping the `ev_*` API/ABI intact:

- **`io_uring` backend rewritten from scratch** — kernel-ABI based, ring-layout
  validation, CQ-overflow recovery, `MAP_POPULATE` fallback, bounded budgets, full
  cleanup on every failure path.
- **Real native-Windows support** — `epoll` via IOCP/wepoll with a thread-safe
  `SOCKET ↔ fd` registry, so `ev_io` watches native winsock handles (upstream's
  `_osfhandle` aliasing never actually worked).
- **macOS uses `kqueue`** instead of `select` — no `FD_SETSIZE` ceiling, O(active)
  scaling. `select` is a strict last resort everywhere.
- **Genuine bug fixes** — use-after-close in `ev_loop_destroy`, dropped kqueue
  registrations on EINTR, broken win32 `accept()` detection, port/linuxaio fd leaks —
  plus integer-overflow, alignment, fd-range and EINTR hardening throughout.
- **5 real wepoll fixes**, consistent `EPOLLRDHUP` TCP half-close across backends,
  and a compile-time ABI contract pinning the watcher layout.
- **Warning-clean** under strict GCC/Clang, passes ASan/UBSan, builds with CMake
  **and** autotools, static **and** shared, on Linux / macOS / Windows.

**See the [CHANGELOG](CHANGELOG.md) for the complete, file-by-file list of every change.**

## Building & testing standalone

### CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /usr/local   # optional
```

Useful options:

| Option                     | Default | Meaning                                            |
|----------------------------|---------|----------------------------------------------------|
| `BUILD_SHARED_LIBS`        | `OFF`   | Build a shared `libev` (`.so`/`.dylib`) vs static. |
| `QB_EV_STRICT_WARNINGS` | `ON`    | Build with the strict warning set.                 |
| `QB_EV_USE_TIMERFD`     | `ON`    | Enable Linux `timerfd` (time-jump detection).      |
| `QB_EV_BUILD_TESTS`        | `ON`*   | Build the watcher coverage test (`ctest`).         |
| `QB_EV_BUILD_BENCHMARKS`   | `ON`*   | Build the cross-backend benchmark.                 |
| `BUILD_PIC_STATIC_LIBS`    | `ON`    | Position-independent static archive.               |

<sub>* default `ON` when qb-ev is the top-level project, `OFF` when embedded via `add_subdirectory`.</sub>

### autotools

```sh
./autogen.sh        # needs autoconf/automake/libtool
./configure
make
```

The man page `ev.3` is generated from `ev.pod` via `pod2man`.

### Tests & benchmark

```sh
ctest --test-dir build --output-on-failure   # watcher coverage test
./build/qb-ev-bench-backends                  # sweeps backends, prints the best one
```

`tests/test-watchers.c` exercises **every** watcher type (io, timer, periodic,
idle, prepare, check, signal, child, stat, embed, fork, async, cleanup, `ev_once`).
`bench/bench-backends.c` runs two workloads (all-active dispatch, and the
`O(N)`-vs-`O(active)` "active-few" discriminator) across every supported backend
and prints which backend to prefer on the current machine. CI
(`.github/workflows/ci.yml`) runs the full matrix on Linux (gcc + clang) and macOS.

## Threading model

Like libev, a single `ev_loop` is **not** thread-safe and must be driven by one
thread at a time — the idiomatic pattern is **one loop per thread**, with
cross-thread wakeups delivered through an `ev_async` watcher. This is also why the
Windows `epoll`/wepoll backend is safe here: wepoll's port is used in its intended
single-threaded-per-loop mode.

## Performance

The backend selection exists for one reason: scaling with the number of watched
descriptors. `select`/`poll` scan **all** registered fds on every wait (`O(N)`),
while `epoll`/`kqueue`/`io_uring`/event-ports report only the *ready* ones
(`O(active)`). With thousands of mostly-idle connections the difference is a cliff,
not a constant — which is why `qb-ev` defaults to the scalable backend on every
platform and treats `select` as a fallback. (The bundled benchmark
`bench/bench-backends.c` makes this visible: on macOS `kqueue` stays flat to tens
of thousands of fds while `poll` collapses; on Linux `epoll` and `io_uring` are
flat and comparable for readiness loops, which is why `io_uring` is not
auto-selected.)

## Compatibility & ABI

`qb-ev` is API- and ABI-compatible with libev 4.x. The `ev_*` symbols, struct
layouts, watcher semantics and the `event.h` libevent compatibility layer are
preserved; only `ev_version_major()` now reports `5`. Existing libev consumers
relink without source changes.

## FAQ

**Is it really a drop-in replacement for libev?**
Yes. The `ev_*` API, struct layouts and watcher semantics are unchanged, and the
`event.h` libevent compatibility shim is kept. Only `ev_version_major()` reports
`5`. Code that compiled against libev compiles against `qb-ev`.

**libev is BSD-2 — how can qb-ev be MIT?**
The combined work is distributed under MIT. Portions derived from libev and wepoll
remain under their original BSD-2-Clause terms; those notices are preserved in
every relevant source file and reproduced in
[THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). MIT and BSD-2 are both permissive and
compatible.

**Why isn't `io_uring` the default on Linux?**
For *readiness-style* event loops (the libev model), `io_uring` is not measurably
faster than `epoll` and adds complexity and kernel-version sensitivity. It is built
when available and can be selected explicitly (`EVBACKEND_IOURING`), but `epoll`
remains the sane default.

**What happens on old Windows / when a backend is missing?**
Selection always falls back gracefully, ending at `select`. `ev_loop_new(backend)`
returns `NULL` if a specific backend cannot be created, so you can probe and react.

**Does it need any dependencies?**
No. It is a single self-contained C library (wepoll is vendored on Windows). The
optional C++ wrapper is header-only (`ev++.h`).

## Credits

`qb-ev` stands on the shoulders of excellent prior work and gratefully credits
its original authors:

- **libev** — © Marc Alexander Lehmann. <http://software.schmorp.de/pkg/libev.html>
- **wepoll** (epoll for Windows) — © Bert Belder. <https://github.com/piscisaureus/wepoll>
- **libevent** compatibility layer — derived from work © Niels Provos.

See [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES) for the full upstream license texts.

## Contributing

Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for build/test
instructions and the PR checklist, [CHANGELOG.md](CHANGELOG.md) for the release
history, and [SECURITY.md](SECURITY.md) to report a vulnerability privately. All
participants are expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

MIT — see [LICENSE](LICENSE). Portions derived from libev and wepoll are retained
under their original BSD-2-Clause licenses (preserved per-file and in
THIRD-PARTY-NOTICES).
