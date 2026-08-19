# qev

[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![version](https://img.shields.io/badge/version-5.0-brightgreen.svg)](CHANGELOG.md)
![C99](https://img.shields.io/badge/C-99-00599C.svg)
![platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20BSD%20%7C%20illumos-lightgrey.svg)
![build](https://img.shields.io/badge/build-CMake%20%7C%20autotools-orange.svg)

**A high-performance event loop for C — libev's API, maintained.**

`qev` is an event loop library: you register interest in file descriptors, timers,
signals and child processes, and it tells you when something happened, using the best
mechanism the operating system offers. It speaks libev's API — `ev_run`, `ev_io_start`,
`ev_timer_init`, `struct ev_loop` — because that API is good, widely known, and
thoroughly documented. It is version **5**, and it picks up where libev 4.33 stopped.

```c
#include <qev/ev.h>

static void on_readable(struct ev_loop *loop, ev_io *w, int revents) {
    /* the socket has data */
}

int main(void) {
    struct ev_loop *loop = ev_default_loop(0);   /* picks epoll / kqueue / event ports / wepoll */
    ev_io w;
    ev_io_init(&w, on_readable, sockfd, EV_READ);
    ev_io_start(loop, &w);
    return ev_run(loop, 0);
}
```

**Coming from libev?** Change one include path. That is the whole migration — the C
API is unchanged, name for name and signature for signature.

**Writing C++?** qev is the loop underneath the [qb Actor Framework](https://github.com/isndev/qb),
which adds an actor engine, coroutine-based async I/O, and HTTP/2·3, WebSocket, PostgreSQL
and Redis modules on top. [More below](#need-more-than-an-event-loop).

---

## Why this exists

libev is one of the best event loops ever written, and it stopped. The last release
was 4.33 in 2020, and the platforms moved on underneath it:

- **macOS and the BSDs** were served by `select` — capped at `FD_SETSIZE` (1024
  descriptors) and `O(N)` on every wait. `qev` uses **`kqueue`**.
- **Windows** had `select` and nothing else. `qev` ships a real **`epoll`** backend
  built on IOCP (via wepoll) with a correct `SOCKET ↔ fd` registry, so `ev_io` watches
  native winsock handles. Upstream's `_osfhandle` aliasing never actually worked.
- The **`io_uring`** backend was incomplete. It has been rewritten against the kernel
  ABI, with ring-layout validation, CQ-overflow recovery and bounded budgets.
- A number of **real bugs** sat behind rarely-exercised paths: a use-after-close in
  `ev_loop_destroy`, kqueue registrations dropped on `EINTR`, broken win32 `accept()`
  detection, descriptor leaks in the event-ports and linuxaio backends.

`qev` is where those fixes live, and where the next ones will. The tree builds
warning-clean under a strict GCC/Clang flag set and passes ASan/UBSan.

## Table of contents

- [Install](#install)
- [Backends per platform](#backends-per-platform)
- [Using it](#using-it)
- [Choosing a backend at runtime](#choosing-a-backend-at-runtime)
- [What is new since libev 4.33](#what-is-new-since-libev-433)
- [Building from source](#building-from-source)
- [Packaging](#packaging)
- [Coexisting with libev](#coexisting-with-libev)
- [Threading model](#threading-model)
- [Performance](#performance)
- [Need more than an event loop?](#need-more-than-an-event-loop)
- [FAQ](#faq)
- [Credits](#credits)
- [License](#license)

## Install

### CMake — installed package

```cmake
find_package(qev 5 REQUIRED)
target_link_libraries(myapp PRIVATE qb::ev)   # `qev::qev` is also provided
```

### CMake — vendored or fetched

```cmake
add_subdirectory(qev)                 # or:
include(FetchContent)
FetchContent_Declare(qev GIT_REPOSITORY https://github.com/isndev/qev.git GIT_TAG v5.0.0)
FetchContent_MakeAvailable(qev)
target_link_libraries(myapp PRIVATE qb::ev)
```

### pkg-config

```sh
cc myapp.c $(pkg-config --cflags --libs qev) -o myapp
```

No dependencies. One self-contained C library; wepoll is vendored on Windows, and the
optional C++ wrapper is header-only.

## Backends per platform

| Platform               | Default (auto)        | Also available                          | Last resort |
|------------------------|-----------------------|-----------------------------------------|-------------|
| Linux                  | `epoll`               | `io_uring`, `linuxaio` (opt-in), `poll` | `select`    |
| macOS / FreeBSD / *BSD | `kqueue`              | `poll`                                  | `select`    |
| Solaris / illumos      | event `port`          | `poll`                                  | `select`    |
| Windows                | `epoll` (wepoll/IOCP) | —                                       | `select`    |

Selection always prefers the most scalable mechanism available and reaches `select`
only when nothing else can be created. `io_uring` and `linuxaio` are compiled when the
kernel headers are present but are **not** auto-selected — see the FAQ.

## Using it

### C

```c
#include <qev/ev.h>
#include <stdio.h>

static void timer_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    puts("tick");
    ev_break(loop, EVBREAK_ONE);
}

int main(void) {
    struct ev_loop *loop = ev_default_loop(0);
    ev_timer t;
    ev_timer_init(&t, timer_cb, 1.0, 0.0);
    ev_timer_start(loop, &t);
    ev_run(loop, 0);
    return 0;
}
```

### C++

The header-only wrapper gives every watcher a type with member-function callbacks.

```cpp
#include <qev/ev++.h>

ev::timer t;
t.set<my_class, &my_class::on_timeout>(this);
t.start(1.0, 0.0);
ev::get_default_loop().run();
```

### Reference

The complete API manual is [`docs/ev.pod`](docs/ev.pod) — libev's own, which applies
verbatim. `make install` from the autotools build renders it as `qev.3`.

## Choosing a backend at runtime

```c
struct ev_loop *loop = ev_loop_new(EVBACKEND_KQUEUE);   /* NULL if unavailable */
if (!loop) loop = ev_loop_new(EVFLAG_AUTO);             /* let qev decide */
printf("using backend 0x%x of 0x%x supported\n", ev_backend(loop), ev_supported_backends());
```

`ev_loop_new()` returns `NULL` when a specific backend cannot be created, so probing
and falling back is a two-line pattern rather than a build-time decision.

## What is new since libev 4.33

Around **70 substantive changes**, plus a hardened wepoll 1.5.8. Semantics and struct
layouts are libev's — this is maintenance, not a redesign.

- **`io_uring` rewritten from scratch** — kernel-ABI based, ring-layout validation,
  CQ-overflow recovery, `MAP_POPULATE` fallback, bounded drain/spin/EINTR budgets, and
  full cleanup on every init-failure path.
- **Real native Windows support** — `epoll` via IOCP/wepoll with a thread-safe
  `SOCKET ↔ fd` registry, plus five wepoll fixes of our own.
- **`kqueue` on macOS and the BSDs** instead of `select`: no `FD_SETSIZE` ceiling,
  `O(active)` scaling.
- **`EPOLLRDHUP` TCP half-close** reported consistently across backends.
- **Genuine bug fixes** — use-after-close in `ev_loop_destroy`, kqueue registrations
  dropped on `EINTR`, broken win32 `accept()` detection, event-port and linuxaio
  descriptor leaks.
- **Hardening throughout** — signed-overflow, alignment, fd-range and `EINTR` fixes;
  strict-aliasing-safe accessors; a compile-time ABI contract pinning watcher layout.
- **Modern build** — CMake package config, pkg-config, CPack, component-aware install,
  autotools kept and working; SPDX headers; MIT.

[CHANGELOG.md](CHANGELOG.md) carries the complete, file-by-file list.

## Building from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

| Option                     | Default | Meaning                                              |
|----------------------------|---------|------------------------------------------------------|
| `BUILD_SHARED_LIBS`        | `OFF`   | Shared `libqev.so`/`.dylib` instead of a static one.  |
| `BUILD_TESTING`            | `ON`    | Standard CMake switch; gates the test target.         |
| `QB_EV_STRICT_WARNINGS`    | `ON`    | Strict GCC/Clang warning set.                         |
| `QB_EV_USE_TIMERFD`        | `ON`    | Linux `timerfd` for time-jump detection.              |
| `QB_EV_WATCHERS_FULL`      | `ON`\*  | All fourteen watcher families.                        |
| `QB_EV_LIBEVENT_COMPAT`    | `OFF`   | Build the libevent shim — see [below](#coexisting-with-libev). |
| `QB_EV_BUILD_BENCHMARKS`   | `ON`\*  | Cross-backend benchmark.                              |
| `BUILD_PIC_STATIC_LIBS`    | `ON`    | Position-independent static archive.                  |

<sub>\* `ON` when qev is the top-level project, `OFF` when embedded via `add_subdirectory`.</sub>

autotools is maintained alongside CMake:

```sh
./autogen.sh && ./configure && make && make install
```

## Packaging

Everything a distribution needs is generated rather than curated by hand.

- **Two install components.** `qev_Runtime` carries the versioned shared object and its
  SONAME symlink — what a *program* needs. `qev_Development` carries the headers, the
  static archive, the unversioned link symlink, the CMake package and `qev.pc` — what a
  *build* needs. That is the `libqev` / `libqev-dev` split, drawn by the build rather
  than by a packager reading a file list.

  ```sh
  cmake --install build --prefix /usr --component qev_Runtime
  cmake --install build --prefix /usr --component qev_Development
  ```

- **CPack**, binary and source:

  ```sh
  cd build && cpack -G TGZ                            # per-component archives
  cpack --config CPackSourceConfig.cmake -G TGZ       # source release
  ```

- **A Debug and a Release build share a prefix** — `DEBUG_POSTFIX` makes the debug
  archive `libqevd.a`, so the second install cannot silently overwrite the first.

- **pkg-config** (`qev.pc`) and a **CMake package config** describe the same thing: both
  resolve `#include <qev/ev.h>`.

## Coexisting with libev

An installed `qev` never overwrites an installed libev. Every path is ours:

| | libev | qev |
|---|---|---|
| archive | `libev.a` | `libqev.a` |
| headers | `include/ev.h` | `include/qev/ev.h` |
| pkg-config | `libev.pc` | `qev.pc` |
| CMake package | — | `find_package(qev)` |
| man page | `ev.3` | `qev.3` |
| include guards | `EV_H_` | `QB_EV_*` |

so one translation unit can hold both `<qev/ev.h>` and a real `<ev.h>` without either
silently disappearing.

> **What is not supported is *linking* qev and a real libev into the same program.**
> Both export the same `ev_*` symbols, and libev is a single translation unit — so an
> archive is all-or-nothing. A program needing symbols from both fails to link with
> `duplicate symbol`; a program touching only the symbols both provide links silently
> and resolves them from whichever archive came first on the command line. Pick one.

The **libevent** compatibility layer (`event.h`) is kept but **off by default**: it
exports 24 *unprefixed* upstream libevent symbols, and libevent is far more widely
deployed than libev. Turn it on with `-DQB_EV_LIBEVENT_COMPAT=ON` and own those names
deliberately.

## Threading model

A single `ev_loop` is **not** thread-safe and must be driven by one thread at a time.
The idiomatic shape is **one loop per thread**, with cross-thread wakeups delivered
through an `ev_async` watcher. This is also why the Windows backend is sound: wepoll's
port is used in its intended single-threaded-per-loop mode.

## Performance

Backend selection exists for one reason: scaling with the number of watched
descriptors. `select` and `poll` scan **every** registered fd on each wait (`O(N)`);
`epoll`, `kqueue`, event ports and `io_uring` report only the *ready* ones
(`O(active)`). With thousands of mostly-idle connections that is a cliff, not a
constant.

`bench/bench-backends.c` makes it visible: it runs an all-active dispatch workload and
an "active-few" discriminator across every supported backend and prints which to prefer
on the current machine. On macOS `kqueue` stays flat into the tens of thousands of
descriptors while `poll` collapses; on Linux `epoll` and `io_uring` are flat and
comparable for readiness loops — which is why `io_uring` is not auto-selected.

## Need more than an event loop?

qev is the event-loop core of the **[qb Actor Framework](https://github.com/isndev/qb)** —
a C++20 framework for building concurrent, network-facing services on top of exactly this
loop.

Where qev gives you readiness notifications, qb gives you what people usually build next
with them, already done and tested:

- **An actor engine.** Lock-free message passing across cores, one loop per thread, no
  shared mutable state to reason about.
- **Async I/O with C++20 coroutines.** `co_await` a socket, a timer, a query. TCP, UDP,
  TLS, QUIC, files, with transports and protocols that compose.
- **Protocol modules.** HTTP/1.1, HTTP/2, HTTP/3 and WebSocket; PostgreSQL; Redis —
  each a first-class library, not a sample.

If you are writing C and want an event loop, qev is the whole answer. If you are writing
C++ and find yourself about to write a connection manager, a thread pool and a protocol
parser around it, qb has spent years on that already.

**The two evolve together.** qb embeds a build of this source, so a fix landing in either
project reaches both — a backend bug found under qb's test suite is a qev release, and a
qev improvement shows up in the next qb. They ship on their own schedules; neither waits
for the other.

## FAQ

**Do I have to change my libev code?**
One include path. `#include <ev.h>` becomes `#include <qev/ev.h>`. Every function,
type, macro and struct field keeps its name and meaning.

**Is it binary-compatible with libev 4.x?**
No, and it could not honestly claim to be. `ev_version_major()` reports 5, and libev's
own documented check is `assert(ev_version_major() == EV_VERSION_MAJOR)` — which a 4.x
consumer fails by construction. Recompile; do not swap the object.

**libev is BSD-2 — how can qev be MIT?**
The combined work is distributed under MIT. Portions derived from libev, wepoll and
libevent remain under their original BSD-2-Clause terms, preserved per-file and
reproduced in [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). Both licences are permissive
and compatible.

**Why isn't `io_uring` the default on Linux?**
For *readiness-style* loops — the libev model — `io_uring` is not measurably faster
than `epoll`, and it adds complexity and kernel-version sensitivity. It is built when
available and selectable with `EVBACKEND_IOURING`; `epoll` remains the sane default.

**What happens when a backend is unavailable?**
Selection degrades gracefully, ending at `select`. `ev_loop_new(backend)` returns `NULL`
if that specific backend cannot be created, so you can probe and react.

**Does it need any dependencies?**
No. wepoll is vendored for Windows; the C++ wrapper is header-only and keeps the `ev::`
namespace — a C++ namespace nests, so it never had the collision the C symbols did.

## Credits

qev stands on excellent prior work and credits its authors:

- **libev** — © Marc Alexander Lehmann. <http://software.schmorp.de/pkg/libev.html>
- **wepoll** (epoll for Windows) — © Bert Belder. <https://github.com/piscisaureus/wepoll>
- **libevent** compatibility layer — derived from work © Niels Provos.

See [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES) for the full upstream licence texts.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build and test instructions and the PR
checklist, [CHANGELOG.md](CHANGELOG.md) for the release history, and
[SECURITY.md](SECURITY.md) to report a vulnerability privately. All participants follow
the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

MIT — see [LICENSE](LICENSE). Portions derived from libev and wepoll are retained under
their original BSD-2-Clause licences, preserved per-file and in THIRD-PARTY-NOTICES.
