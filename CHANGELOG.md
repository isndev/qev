# Changelog

All notable changes to **qev** are documented in this file. The format is based
on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to
[Semantic Versioning](https://semver.org/).

## [Unreleased]

Nothing yet. Entries land here as they are merged, and move under a version heading when that version is tagged.

## [5.2.0] — 2026-10-25

### Changed

- **`EVFLAG_NOSIGMASK` leaves the signal mask alone, and without it a watched signal is unblocked
  (Huly QB-713).** The test in `ev_signal_start` was inverted, as in libev 4.33 (libev's CVS has
  reversed it since: "the behaviour of EV_NOSIGMASK was reversed"): with the flag, starting the
  first watcher for a signal unblocked it in the calling thread -- the one modification the flag
  exists to prevent, on a signal its caller keeps blocked on purpose (a sigwait thread feeding
  `ev_feed_signal`) -- and without it the mask was never touched, though the manual says libev
  unblocks. Now the default unblocks, as documented, and the flag never calls `sigprocmask`, the
  signalfd path included (its block at the start, its unblock at the stop): with
  `EVFLAG_SIGNALFD | EVFLAG_NOSIGMASK` the caller keeps the signals blocked itself. `docs/ev.pod`
  says so. Witness `test_signal_mask` (`tests/test-watchers.c`, three cases, red before).

### Added

- **`ev_periodic_count_addr(loop)`: the number of active periodics, read inline (Huly QB-275).**
  `const int *ev_periodic_count_addr(EV_P)`, declared beside the other `ev_periodic_*` under
  `EV_PERIODIC_ENABLE` and valid until `ev_loop_destroy`: the counterpart of `ev_timer_count_addr`.
  `ev_timer_next` already folds the earliest periodic into its deadline, but an embedder that asks
  for it only while `*ev_timer_count_addr` is non-zero saw a loop holding periodics and no timer
  as having nothing to fire -- qb's listener gated its non-blocking pass that way. Listed in
  `Symbols.ev` and `docs/ev.pod`. Every other function of `ev.c`, `ev_run` included, compiles to
  the same instructions (MSVC x64 listing; g++-14 objdump, alignment padding aside). Exports:
  86 `ev_*` on Linux -- 85 functions and the `ev_default_loop_ptr` variable, the count the build
  matrix's census prints since QB-1071 -- and 85 on Windows; qb's embedded profile 73 on Linux,
  74 on Windows. Test `test_periodic_count` (`tests/test-loops.c`: start,
  again, a one-shot firing and stopping itself, stop; unconditional floor 52 -> 58).
- **`ev::loop_ref::periodic_count_addr()` in `ev++.h` (Huly QB-275).** The wrapper's twin of
  `timer_count_addr()`, under `EV_PERIODIC_ENABLE`: the address `ev_periodic_count_addr` answers. qb's
  listener reads it beside the timer count to judge a periodic's deadline on a non-blocking pass. Test
  `test_count_addrs` (`tests/test-cxx.cpp`, both profiles: the address, and the count following
  `ev::periodic` start and stop while the timer count stays 0).

### Fixed

- **The wake pipe says why it ignores two results, and the `-Wno-unused-result` mask is gone (Huly
  QB-1079).** `evpipe_write`'s `write()` and `pipecb`'s drain `read()` ignore their result on purpose
  -- a non-blocking write fails only with `EAGAIN` when a wake is already pending, a signal handler
  can do nothing safe about anything else, and a drain fails only on an empty pipe -- but nothing in
  the code said so: the library target masked the warning with `-Wno-unused-result`, which hid every
  other ignored result too, and a build without that mask (the white-box `tests/test-linux-internals.c`,
  or an embedder compiling `ev.c` itself) under a fortified glibc got four `-Wunused-result` warnings
  (gcc 13, Ubuntu 24.04). The four results are now taken and discarded explicitly, with the reason
  beside `evpipe_write`, and the mask is removed. The object code is unchanged instruction for
  instruction (162/162 functions under gcc 14, 135/135 under clang 19). The build matrix also
  compiles its hand-built C++ suite as `-std=c++17`, like the CMake target, and a red cell now prints
  its first warnings.
- **Windows: the `SOCKET <-> fd` registry never writes past its free list (Huly QB-315).** The free
  list grew on a release, announcing its new size before a `realloc` whose failure the release
  ignored: the next release wrote through NULL (first growth) or past the old block (later
  growth). Both tables now grow together, geometrically, in `ev_win32_fd_reserve`, each size
  committed only after its `realloc` succeeded; the free list can then hold every index ever
  issued, so a release never allocates, and a failed acquire returns `ENOMEM` with nothing
  consumed. Witness: the new white-box suite `tests/test-win32-internals.c` (it compiles `ev.c` in,
  with an allocator that fails on demand, and checks every announced size against `_msize`).
- **Windows: stopping one of several watchers on a socket no longer deregisters the others
  (Huly QB-317).** `ev_io_stop` deleted the socket's wepoll registration unconditionally; with
  another watcher left on it and the aggregate mask unchanged, `fd_reify` never re-added it, and
  the survivor stayed active with no event ever. The eager delete now happens for the last
  watcher only (it keeps protecting a recycled fd from a stale registration) and resets the fd's
  recorded kernel state, so a watcher restarted before the next pass -- with or without
  `ev_io_set_sock` -- is added again; that restart was lost the same way. Witness
  `test_shared_socket` (`tests/test-wepoll.c`, floor 13 -> 17).
- **Windows: the clock caches are initialised once (Huly QB-337).** Every thread that read a
  clock cold resolved `GetSystemTimePreciseAsFileTime` and computed the `QueryPerformanceCounter`
  scale and stored them, unsynchronised: identical values, a data race all the same, reachable
  by concurrent `ev_loop_new`. They are set once under `InitOnceExecuteOnce` with release stores
  and tested with acquire loads -- a plain `mov` on x86-64 -- each cache a union of its value and
  the integer image the atomics move, the cold path out of line. MSVC x64 listings, base against
  fix: `ev_run` identical; the QPC read two instructions shorter (an integer test replaces the FP
  compare and its parity branch; the multiply reads the same global). Witness
  `test_clock_cold_start` in the white-box suite (four cold threads: four concurrent initialisers
  before, one after).
- **Windows: wepoll's "initialised" flag publishes its initialisation (Huly QB-340).**
  `init()` -- on every `epoll_*` call -- skipped `InitOnceExecuteOnce` when a plain `bool` said the
  initialisation completed, with no happens-before with what it set up. It is an acquire load of
  a flag the once-callback release-stores: a plain load on x86-64 (MSVC: `mov` + `test` where
  `cmp BYTE PTR` was, one instruction at the entry of calls that then wait on IOCP), an `ldar` on
  arm64. No race detector exists for MSVC and x86-64 cannot exhibit it: the concurrent cold start
  (`test_concurrent_loop_new`, now on Windows too) is a functional check, not its witness.
- **`ev_now_set` keeps a due periodic due across its realtime refresh (Huly QB-316).** Every half
  `MIN_TIMEJUMP` the supplied clock's pass reads the wall clock again, and it recomputed every
  periodic -- moving an interval or rescheduled one that had fallen due in that half second past
  the pass that should fire it. It now recomputes them only when the read shows a wall-clock jump,
  the test `time_update` makes, in an out-of-line refresh: the per-pass path compiles to the
  same instructions (MSVC x64 listing). Witness `test_now_set_periodic` (`tests/test-loops.c`).
- **The realtime timerfd recomputes the periodics only on a wall-clock jump (Huly QB-1055).** With
  `EV_USE_TIMERFD` (off by default; `QB_EV_USE_TIMERFD`), the timerfd also turns readable when its
  own deadline, `MAX_BLOCKTIME2` (about seventeen days) ahead, runs out, when it is first armed,
  and after a fork; its callback recomputed every periodic on each of those wakes, so one due but
  not yet delivered was pushed past its fire and every rescheduler was asked again. It now applies
  the jump test of `time_update`, measured afresh -- one macro, `time_jumped`, shared with
  `ev_now_set`'s refresh. The default build compiles to the same instructions, every function of
  `ev.c` (g++-14 objdump, MSVC listing). Witness: the new white-box suite
  `tests/test-linux-internals.c` (Linux; `ev.c` compiled in with the timerfd forced on, the callback
  driven as the loop drives it): 2 FAIL before, 4/4 after.
- **`ev_async_pending` reads `sent` with an acquire load (Huly QB-724).** It was a plain read of a
  flag `ev_async_send` sets from another thread with a release store; ThreadSanitizer reported the
  owner's quick check as a data race. The macro now makes the access `EV_WAKE_LOAD` makes, under
  the same `EV_NO_ATOMIC_BUILTINS` opt-out; `ev_async` and the C99 floor are unchanged, and every
  line of `ev.h` before the helper keeps its number. Witness `test_async_pending_xthread`
  (`tests/test-loops.c`, under TSan).
- **An `ev_once` still waiting when its loop is destroyed is freed (Huly QB-736).** Only its
  callback freed its private allocation; a loop destroyed first (the timeout ahead, the event
  queued but not invoked) kept it. `ev_loop_destroy` now finds each one through the loop's own
  structures (the fd lists, the timer heap, the pending queues), frees it once, and invokes
  nothing. Witness: `test_once` with a counting `ev_set_allocator` (four teardown shapes, two more
  on POSIX; floor 4 -> 8). `tests/test-loops.c`'s unconditional floor is 48 -> 52.
- **libevent compatibility: `event_del` cancels an activation still pending (Huly QB-712).** It
  stopped the timeout only when the timer was active, and `event_active (ev, EV_TIMEOUT, …)` makes
  it pending without starting it, so the callback ran on the next pass after the removal -- and a
  caller that freed the event at once left the loop a pending pointer into freed memory (ASan:
  heap-use-after-free in `ev_invoke_pending` on `b1c7843`, clean after). The io
  watcher of an event with no fd interest, made pending by `event_active (ev, EV_READ, …)`, was
  not stopped either. `event_del` now stops the signal or io watcher and the timer
  unconditionally: a stop clears a pending activation and does nothing more to an inactive
  watcher. `tests/test-event-compat.c`, the first test of the layer, checks each activation kind
  deleted and kept, and a timeout armed then deleted; on `b1c7843` the two deleted activations ran
  their callback once each. The build matrix now builds the opt-in profile
  (`QB_EV_USE_LINUXAIO=ON`, `QB_EV_LIBEVENT_COMPAT=ON`), which no cell had compiled, runs the loop
  suite over it -- on Linux its aio cases must run -- and runs this test.
- **C++ wrapper: three members that never compiled or never did their job (Huly QB-312, QB-313,
  QB-711).** The wrapper is templates, and a member nothing instantiates is never compiled:
  `w (revents)` passed the watcher where the callback takes the loop and did not compile as soon
  as it was used (QB-312) -- it now invokes the callback as `ev_invoke` would, with the watcher's
  loop, the watcher and `revents`; `ev::embed::start (loop)` resolved to the base's `set (loop)`,
  moving the watcher to the loop it was meant to embed and leaving that loop unset (QB-313) -- it
  calls `set_embed` and the watcher stays on its own loop; and under `EV_MULTIPLICITY=0`, the
  single-loop embedding profile `docs/ev.pod` says the wrapper supports, the three callback
  thunks named a `loop` parameter that profile does not have, so `ev++.h` did not compile at all
  (QB-711), and `loop_ref`'s comparisons left a parameter unused. `tests/test-cxx.cpp`
  instantiates and drives every callback form, calls each directly, checks the loop a direct call
  passes on two loops, and embeds a loop with `start (loop)` across a stop and a second start; the
  matrix builds it against the library and in the single-loop profile (`ev.c` compiled in under
  `EV_MULTIPLICITY=0`), warning-free with `-Wall -Wextra` under gcc and clang. `docs/ev.pod`
  documents the direct call and what `start (loop)` does for `ev::embed`.
- **io_uring: a watcher on descriptor 0 fires (Huly QB-336).** Each poll is tagged
  `fd | generation << 32`, and the first registration of descriptor 0 is the tag 0, which the
  completion drain skipped as if it were a sentinel: the readiness was dropped and the one-shot
  poll never re-armed, so a watcher on fd 0 -- stdin, or the first socket of a daemon that closed
  it -- never fired. The one reserved tag is a POLL_REMOVE's own, whose low half is no descriptor.
  `tests/test-loops.c` `test_iouring_fd_zero` makes fd 0 a pipe before the loop exists and needs
  two events, the second only reachable through the re-arm; its control stops the watcher (silent,
  the cancelled poll's old generation ignored) and restarts it (generation 1, one event).
- **io_uring: a loop handed from thread to thread keeps working (Huly QB-744).** `docs/ev.pod`
  (THREADS) lets one loop be called from several threads as long as the calls never overlap, but
  the ring was created `IORING_SETUP_SINGLE_ISSUER`, which binds submission to the creating thread:
  the first SQE another thread submitted failed `EEXIST` and went to `ev_syserr`, an abort by
  default. Without `DEFER_TASKRUN` the flag only recorded and checked that thread (Linux 6.6), so it
  is dropped and `COOP_TASKRUN | TASKRUN_FLAG` stay -- which a 5.19 kernel, refusing the
  then-unknown `SINGLE_ISSUER` (6.0), had dropped too, falling back to no flag at all (not
  measured: the host runs 6.6). The case then found a second way to lose a
  handed-over loop: the kernel completes a poll whose submitting thread has exited `-ECANCELED`
  once its fd is ready, and the drain dropped every `-ECANCELED`, so the watcher went silent for
  good. A current-generation poll that ends without an event -- 0, `-ECANCELED`, `-EINTR`,
  `-ENOENT` -- is now re-armed and resubmitted from the thread running the loop; a cancellation of
  ours still carries an older generation and is still ignored. `test_iouring_thread_handoff` creates
  and arms the loop on one thread, hands it to a second that completes that poll, registers and
  re-arms its own, and takes it back after the second has exited: 2 of 6 rounds and 2 syserr before,
  4 of 6 with the flag alone dropped, 6 of 6 after. The drain tests `res <= 0` once instead of
  twice: callgrind over 10 000 passes, `iouring_cq_drain` level at one completion a pass
  (1 130 000 Ir both) and 14 030 000 → 13 880 000 Ir at sixteen.
- **Linux aio: the backend frees its arrays at teardown and when it falls back to epoll
  (Huly QB-338).** It grows two pointer arrays, the per-fd iocb table and the `io_submit` batch;
  its destroy freed the iocbs but neither array, and its permanent switch to epoll, after which no
  destroy of this backend runs, dropped them too. Both now go through `linuxaio_free_arrays`; a
  fork or a regrow of the aio context keeps them, as before, to reuse their capacity. Three cases in
  `tests/test-loops.c` count live blocks through `ev_set_allocator` with the backend really active
  (`QB_EV_USE_LINUXAIO=ON`): three create/destroy cycles left 6 blocks, a regrow past the first
  context 2, the fallback to epoll -- forced by taking the system aio quota for the passes that
  need it -- 2; each now ends at 0, still delivering on every watcher.
- **Release checks cannot certify cached refs or sweep unrelated files into a version roll
  (Huly QB-717, QB-1038).** The qev train requires fresh develop/main fetches for qev, qb and
  the root, treats ordinary untracked files as dirty, and stages only the six files it owns.
- **Release archives are checked against their platform contents (Huly QB-741).** The release
  workflow rejects notices-only, empty, unsafe or ambiguously named component archives.
  A deterministic macOS post-processing step removes inherited AppleDouble metadata
  sidecars before the archive content check and upload.
  Runtime carries the real loader artifact and loader name on Linux and Darwin, or qev.dll on
  Windows; Development carries `include/qev/ev.h`. Both carry nonempty license notices.
  Download verification requires the exact seven archives and SHA256SUMS coverage, source
  content without extraction, and provenance tied to the release tag and workflow.
- **Build guide describes the CMake switches actually used (Huly QB-738).**
  `BUILD_TESTING` affects the default test target only; standalone benchmark targets
  have their own switch, and the reduced watcher profile still enables async.
- **Build and CI checks report the artifact they actually tested (Huly QB-314, QB-729–735,
  QB-739, QB-740, QB-742, QB-743).**
  Autotools leaves the libevent shim off unless requested; the build tree stages its optional
  headers; pkg-config has separate Release and Debug files and handles nested libdirs. The
  matrix executes its full POSIX suites and rejects a failed reduced suite, a collision with
  pre-existing qb files, and a failed wrap-header preprocessor. Loop and wepoll tests now
  exercise the intended rearm, isolation, concurrent initialization, and backend paths.
  Benchmark labels no longer claim a backend or descriptor count they did not measure.
- **The `ev++.h` and libevent-layer suites run under `ctest`, Windows included (Huly QB-1071).**
  `tests/test-cxx.cpp` and `tests/test-event-compat.c` were built and run only by
  `scripts/test-build-matrix.sh`, which runs on Linux and macOS; the Windows CI job runs `ctest`,
  so neither had ever run on Windows. Both are registered now, as `cxx` and `event-compat`: the
  first links the library, with C++ enabled for that target alone (a host without a C++ compiler
  says so and builds the rest) and built at C++17 -- at MSVC's and clang-cl's C++14 default,
  clang-cl warned that `ev.h`'s and `ev++.h`'s noexcept function-pointer parameters mangle
  differently in C++17; the second links it when `QB_EV_LIBEVENT_COMPAT=ON` and otherwise
  compiles `event.c` in itself, beside the headers that option stages, so every build checks the
  layer. A build sanitized through the C flags alone -- the library is C -- still links the C++
  suite against its instrumented archive: the suite takes the sanitizer options of
  `CMAKE_C_FLAGS` and `CMAKE_C_FLAGS_<CONFIG>`, those alone, for its own compile and link
  (clang++ has no runtime otherwise; g++ pulled it in from C's implicit libraries), and the
  matrix's ASan/UBSan cell, configured that way, now runs it. The matrix keeps its cells -- the
  single-loop `EV_MULTIPLICITY=0` profile, a compile without a warning -- and its full-suites
  cell, like the Windows job, now requires every suite registered (at least 4 on POSIX, 6 on
  Windows, where 4 registered before). The matrix's symbol census counts `.bss` symbols as well: `ev_default_loop_ptr`, the one exported variable, is
  zero-initialised, and the `T`/`D`/`S` filter printed 85 `ev_*` where the archive exports 86,
  the count `Symbols.ev` lists; the line now reads `86 ev_* (85 functions, 1 data)`.
- **`test_clock_resolution` no longer fails on a loaded host (Huly QB-1056).** It opened its 50 ms
  window before taking the loop's first reading and took its last reading before the check that
  closed it, so a preemption at either end left the loop's clock measuring less than the window:
  pinned to two CPUs beside four busy processes, 6 to 10 runs in 200 failed on Linux, 91 in 200
  on Windows. The readings now bracket the window -- the first before it opens, one more after it
  closed -- and the upper bound is the wall time the readings spanned rather than a constant: 0 in
  200 under the same load on both. A clock quantised to the 15.625 ms system tick still fails it.
- **`ev_timer_next`'s documentation states the loop's strict due rule.** `docs/ev.pod` and the comment
  above the function in `ev.c` said a deadline at or before the embedder's reading is due; `timers_reify`
  fires a timer only when its deadline is strictly before the loop's time (`ANHE_at (timers [HEAP0]) <
  mn_now`), so an embedder that gated `ev_run` on the inclusive test ran a pass that fired nothing whenever
  its reading equalled the deadline -- on Windows, where the clock moves in 100 ns steps, the first pass
  past a deadline does as a rule. Documentation only; qb's gate was corrected in qb (Huly QB-1016).

## [5.1.0] — 2026-09-20

### Changed

- **The library a build with no configuration header gets is libev's, not qb's profile.** `ev.h`'s
  own `#ifndef` defaults pinned idle, prepare, check, fork, child, async and embed to 0 -- qb's
  seven-family profile, demoted from libev's `EV_FEATURE_WATCHERS` into a trailing comment. The
  CMake builds never saw it (the generated `ev_config.h` is included first and decides), but the
  autotools build defines no `EV_*` at all, so `./configure && make` shipped a `libqev` without six
  watcher families and without `ev_async`. The defaults are libev's again; `config.h.cmakein` is
  what narrows the embedded profile, and the watcher suite now refuses to compile against a
  standalone library missing a family (`QEV_EXPECT_FULL_WATCHERS`, set from
  `QB_EV_WATCHERS_FULL`).
- **The Linux aio backend is opt-in, as in libev** (`QB_EV_USE_LINUXAIO`, default OFF): the
  presence of `<linux/aio_abi.h>` used to compile it in. The dead `EV_USE_WSAPOLL` switch, which
  no source ever read, is gone from the generated header.
- **The epoll backend asks the kernel for a blocking wait in nanoseconds (Huly QB-196).**
  `epoll_wait` takes whole milliseconds and libev rounds UP (`EV_TS_TO_MSEC`, plus a
  `backend_mintime` of 1 ms), so a wait bounded under a millisecond -- an embedder parking its
  thread for 100 µs, a timer 200 µs away -- slept a full one. Measured in qb 3.2's core on WSL2
  g++-14 (Linux 6.6) with qb-vs-others' `parked-timer-wake` probe: a 100 µs timer on a parked
  core fired 1010 µs late at p50, a 1 ms one 110 µs late, a 5 ms one reached through 1 ms parks
  357 µs late, against 0.1 µs on a spinning core. On Linux 5.11 or newer with a libc that declares
  it (glibc 2.35), `epoll_poll` now waits through `epoll_pwait2`, a `struct timespec` the kernel
  honours to the thread's timer slack (50 µs by default, `prctl(PR_SET_TIMERSLACK)`); the kernel
  is asked once per loop at init (`epoll_have_pwait2`, a loop variable rather than a static so two
  loops initialised on two threads never race), an older one answering `ENOSYS` keeps
  `epoll_wait` and the millisecond minimum, and a NOWAIT poll keeps `epoll_wait` on every kernel
  -- both enter the same path and `epoll_wait` copies nothing in, so the non-blocking pass measured
  at its floor (QB-188) pays nothing. `EV_USE_EPOLL_PWAIT2` is the switch, derived like every
  other `EV_USE_*` (CMake `check_symbol_exists`, autotools `AC_CHECK_FUNCS`, or the glibc version
  when neither ran). Windows compiles the same backend over wepoll, whose `epoll_wait` is the
  millisecond one, and keeps it -- and there the measurement refuted the premise the issue was
  filed on: with the system timer resolution read at 15.625 ms, a 1 ms wepoll wait returned in
  1.0–1.5 ms at p50 and 2.4 ms at p99, `timeBeginPeriod(1)` changing nothing (the kernel's waits
  are tickless; its coalescing is the +0.5–1.5 ms), so no high-resolution timer was pursued. The
  io_uring backend keeps libev's 1 ms `backend_mintime` on its timespec wait, unmeasured -- the
  recorded gap. `tests/test-loops.c` `test_epoll_ns_wait` pins it: a blocking run over a 200 µs
  timer, the best of twenty rounds under 800 µs (257 µs measured), a SKIP where the libc or the
  kernel lacks the call; its negative control -- the nanosecond path switched off after the probe
  -- fails it at 1058 µs.
- **The reduced watcher profile keeps `async`.** `QB_EV_WATCHERS_FULL=OFF` used to compile out
  seven families; it now compiles out six — idle, prepare, check, fork, child, embed — and
  leaves `ev_async_start/stop/send` in. An embedder that parks a thread inside `ev_run` needs
  one entry point another thread may call against that concurrent `ev_run`, and `ev_async_send`
  is the only one libev offers; qb 3.2's `VirtualCore` is that embedder. Cost, measured on
  Linux/x86-64: three exported symbols and 24 bytes of `struct ev_loop` (61 `ev_*` / 496 bytes
  against the full profile's 74 / 592).
- **The non-blocking pass at its floor (Huly QB-188).** An `EVRUN_NOWAIT` pass — what an
  embedder that drives the loop from its own scheduler pays on every pass of a thread that owns
  one watcher — did three things only a pass that may sleep needs: it read the clock twice
  (once to size a sleep it would not take, once after the poll), raised the wake-up handshake
  (`pipe_write_wanted = 1` plus a full memory fence, so that a signal handler or an
  `ev_async_send` on another thread would WRITE the evpipe to end a sleep), and — since the
  evpipe is an `ev_io` — counted that pipe as a pollable fd, so the first signal or async
  watcher a loop ever started re-enabled the backend poll for the life of the loop, undoing the
  fix below for every embedder that parks with `ev_async_send` as its wake. Now a NOWAIT pass
  reads the clock once (after the poll, the read timers and callbacks see), leaves the
  handshake down — a sender then takes the flag path it already had for a loop that is not
  sleeping, and the tail of the pass reads `sig_pending` / `async_pending` directly beside
  `pipe_write_skipped`, so a wake-up byte written around a sleep and landing after its poll
  returned is delivered by the next pass rather than by the next poll — and `ev_io_start` /
  `ev_io_stop` do not count the loop's own `pipe_w`. Measured with `bench/bench-pass.c`
  (i9-12900K): WSL2 g++-14, a NOWAIT pass over a timers-only loop **51 → 22 ns**, over a
  quiet socket 132 → 101, an empty loop 50 → 22; MSVC 19.51, timers-only 19.7 → 15.8. In qb
  3.2's core (`qb-vs-others` `ask-cost`): a `co_await qb::ask<E>()` with a 500 ms timeout
  **174 → 115 ns** per round trip on g++ (−34 %), 124 → 115 on MSVC (−7 %; there the pass was
  already cheap because its clock was the system tick — see the Windows clock fix below), a
  one-chunk `ask_stream` with a timeout 240 → 179 / 327 → 319; the untimed ask, the push and
  the one-core pass do not move. `tests/test-loops.c` pins the delivery contract in both
  directions (`test_nowait_async`, `test_nowait_signal`, `test_nowait_clock`: a send or a
  signal between two NOWAIT passes is delivered by the very next pass with no fd to poll, 200
  cross-thread sends into a NOWAIT-only loop and 200 more across parks interleaved with
  NOWAIT passes each delivered before the next, the evpipe not counted after a park, `ev_now`
  advancing across a NOWAIT pass and an expiring timer judged on that pass's own read; the
  unconditional floor rises 20 → 32 with the clock case below). Each of the two mechanisms
  was disabled in turn and the suite rejected both (7 and 3 checks).

### Added

- **`ev_now_set(loop, mono)`, `ev_clock_now()`, `ev_timer_count_addr(loop)`, `ev_timer_next(loop)`
  and `ev_wake_pending_addr(loop)` (Huly QB-190):** the embedder owns the pass. `ev_now_set` hands
  the loop a reading of its own clock (`ev_clock_now`, the monotonic clock the timers run on), and
  the next `EVRUN_NOWAIT` pass reads none: its tail time update stands down for that one pass
  (timers are judged against the supplied time; an older sample is ignored, the loop's clock never
  steps back; a blocking pass always re-reads; the realtime clock follows the sample as the loop's
  own update derives it). The other three are what an embedder reads, inline, to decide that a
  pass has nothing to do and skip `ev_run` altogether: the active timer count, the earliest
  deadline on `ev_clock_now`'s scale (beyond any reading when none), and the flag an
  `ev_async_send` or a signal raises from another thread while no pass was blocking. With the
  pending counts and the io count that is every reason a non-blocking pass could have to enter
  the loop. Measured (`bench-pass`): a pass over one far timer costs 21.8 ns on g++-14 / 28.6 on
  MSVC; given a free sample (`timer+set0`) 10.3 / 15.0 — the loop's bookkeeping floor; the
  embedder's gate alone (`gate`: count, deadline, flag) 1.8 / 0.8. qb 3.2's cores skip the pass:
  a busy core with a far timer 36.6 → 26.3 ns per pass on g++, a quiet socket between two polls
  48.0 → 27.5. `tests/test-loops.c`: `test_now_set` (a fresh supply does not fire a far timer, a
  supply ahead fires it at once, an older supply is ignored, a supply stands for one pass, a
  blocking pass sleeps and re-reads), `test_timer_next` (count and deadline follow start / stop,
  a reading at the deadline means due), `test_wake_pending` (set by a send between passes,
  cleared by the pass that delivers it); unconditional floor 35 → 48. Exports: 85 `ev_*` on POSIX,
  84 on Windows.
- **`EVRUN_NOPOLL`, `ev_io_count_addr(loop)` and `ev_io_fed_addr(loop)` (Huly QB-191):** the
  embedder owns the io cadence. A non-blocking pass with the flag reifies timers and periodics
  and invokes every pending event exactly as before and only leaves the backend call out; a pass
  that would block ignores it (with a descriptor to wait on, the poll is the wake). The two
  addresses are the per-pass inputs of a cadence, read inline: how many descriptors a poll would
  look at, and how many ready ones the last polls reported (`iofed`, one per `fd_event`; it
  grows and wraps), so an embedder polls on every pass while a descriptor is busy and backs off
  while it is quiet. qb 3.2's cores poll a quiet socket once per microsecond instead of once per
  pass — the poll was the whole of what an io pass cost over a timers-only one (101 vs 22 ns on
  WSL2, `bench-pass`'s `fd` shape). `tests/test-loops.c` (`test_nopoll`): an expired timer and a
  fed event under the flag, a readable fd NOT delivered under it and delivered by the next plain
  pass, a blocking pass ignoring it, and the fed count moving only when a poll found the fd
  (unconditional floor 32 → 35). Exports: 80 `ev_*` on POSIX, 79 on Windows.

- **`ev_io_count(loop)`** (`EV_FEATURE_API`, plus `loop_ref::io_count()` in `ev++.h`): the
  number of active `ev_io` watchers, the loop's own wake pipe excluded — the count the fix
  above reads. Covered by
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

### Fixed

- **The shared object's file version follows the release.** The CMake build derived the library
  `VERSION` as libtool's current.age.revision with age and revision pinned to 0, so every release
  of a major line produced a `libqev.so.5.0.0`: the 5.1.0 archives would have carried a file named
  5.0.0 beside a `SHA256SUMS` saying 5.1.0. The target carries `PROJECT_VERSION` (SONAME
  `libqev.so.5`), and `Makefile.am`'s `VERSION_INFO` is derived from the version the same way
  (current = major + minor, revision = patch, age = minor), so both build systems name the object
  `libqev.so.X.Y.Z`. The release train that checks it is `dev/agent/qev-release.sh` in qb's
  development tree: it verifies a published release by download, the file name included.
- **The CMake build no longer dies on a Windows host that has Git for Windows' `pod2man` on its
  PATH (Huly QB-211).** `find_program(pod2man)` accepted `Git/usr/bin/core_perl/pod2man` -- a
  `#!/usr/bin/perl` script with no extension, which CreateProcess cannot start -- and the
  `qev-man` target being `ALL`, `cmake --build` stopped at "Generating qev.3" with the library
  and the three test binaries never built (ctest: 3 "Not Run"); the same tree configured, built
  and passed 3/3 the moment that directory left the PATH, which is the CI runner's shape and why
  `windows-msvc` never saw it. Running the script through the MSYS perl beside it is no answer
  either (from cmd.exe its @INC is MSYS paths: "Can't locate Pod/Man.pm"). On Windows only a
  native `pod2man` (`.exe`/`.bat`/`.cmd`, Strawberry Perl's for instance) renders the page now; a
  script is reported at configure time and the page skipped -- it is decorative, the library is
  the deliverable. POSIX is unchanged: the kernel runs the shebang and `qev.3` is generated and
  installed as before. Measured on Windows 11 / MSVC 19.51, both PATH shapes, same session.

- **io_uring: from 47× slower than epoll on a quiet fd to parity (Huly QB-81).** Measured for
  the first time against `epoll` on the same loop, the io_uring backend ran an embedder's
  non-blocking pass over one quiet socket at **1345 ns against 28.5** — a self-perpetuating
  syscall storm. `iouring_poll` armed its deadline timerfd at "now" on every timeout-0 poll
  (`if (timeout >= 0.)`), the timerfd expired at once, its one-shot `POLL_ADD` completed,
  `iouring_tfd_cb` drained it, the `POLL_ADD` had to be re-armed through `io_uring_enter`, and the
  next pass armed "now" again: three syscalls a cycle, ~300k cycles a second, whatever the
  embedder's cadence (upstream libev carries the same `>=`; nobody drives it at millions of
  non-blocking passes a second). Three changes. The timerfd is armed only for a poll that will
  SLEEP (`> 0.`) — it is the wake-up of a blocking wait, and libev judges its timers on `mn_now`
  every pass: a quiet pass 1345 → 25.8 ns at a 1 µs cadence (epoll 28.3), 1303 → 39.8 when polled
  on every pass (epoll 124.0), one `io_uring_enter` a second where there were 300k. That exposed
  the second defect: the ring is created `COOP_TASKRUN`, which defers the kernel's completion
  work to the task's next syscall, and a loop that now makes none saw a ready fd only at the
  scheduler tick — wake p50 2.0 ms against epoll's 3.9 µs; the storm had been making that syscall
  by accident. `IORING_SETUP_TASKRUN_FLAG` makes the kernel raise `IORING_SQ_TASKRUN` when such
  work is pending, and the post-drain flush enters (`GETEVENTS`) on that flag as on CQ overflow
  and drains again in the same pass — a memory read when quiet, one syscall when something
  completed; both flags or neither (the setup fallback drops the set). Third, `iouring_tfd_w`,
  the `ev_io` the loop starts on its own timerfd, was counted in `iocnt`, so a timers-only loop
  never had `ev_io_count() == 0` and the no-poll pass of QB-187 and the inline gate of QB-190
  never applied under io_uring: like the wake pipe since QB-188, it is the loop's own —
  `io_is_loop_own()` keeps both out of the count for `ev_io_start` / `ev_io_stop` / `ev_walk`.
  Final figures against epoll (WSL2 6.6, g++-14, one core pinned, medians): quiet-socket pass
  28.8 / **26.4** ns at a 1 µs cadence, 126.9 / **40.9** on every pass, timers-only 26.5 / 26.4,
  wake p50 3.98 / 4.18 µs, a parked thread woken by a socket p50 41.6 / 41.3 µs. Parity with a
  different shape — no syscall on a quiet pass, ~0.2 µs more per delivered event, two syscalls
  per park — and epoll stays the default. Two Linux tests, skipping cleanly where io_uring is out:
  `test_iouring_quiet_nowait` (200k NOWAIT passes over a quiet pipe end on io_uring against the
  same on epoll, ratio ≤ 2; the storm measured 47×) and `test_iouring_blocking_timer` (the
  positive control that the sleeping path still arms its deadline). `LIBEV_FLAGS` selects a
  backend for the standalone suite; qb runs its whole suite on the backend under both
  sanitizers.
- **The wake protocol's cross-thread flags are atomic accesses, and ThreadSanitizer says so
  (Huly QB-192).** `pipe_write_wanted` / `pipe_write_skipped`, `sig_pending` / `async_pending`,
  `ev_async.sent`, `signals[].pending` and `.loop`, and the file-scope `have_realtime` /
  `have_monotonic` every `ev_loop_new` writes were `EV_ATOMIC_T` (`volatile sig_atomic_t`)
  accesses fenced by hand — correct on every target libev shipped on, and a data race by the
  letter of C11 that TSan reported as such at every `ev_async_send` across threads (fifteen
  warnings for eight threads creating loops at once, `test_concurrent_loop_new`). Every such
  access goes through `EV_WAKE_LOAD` / `EV_WAKE_STORE_REL` / `EV_WAKE_STORE_RLX` now:
  `__atomic_load_n` / `__atomic_store_n` on GCC and clang (opt out with
  `EV_NO_ATOMIC_BUILTINS`), the original access token for token on MSVC; the fences stay (strictly
  more ordered), the C99 dialect and the `ev.h` ABI are untouched (no `_Atomic`). Cost nil,
  proven by codegen: g++ -O3 emits the same `movl` for the acquire load and the release/relaxed
  stores as for the volatile access, byte for byte, and `cl /EP` expands the macros to the
  original access. Left plain, each justified in place: `loop_done` (`ev_break` is owner-thread
  only for an embedder), the pre-publication inits of `loop_init` / `ev_async_start`. Found
  because qb's sanitizer presets finally instrument the embedded `ev.c` (its target had never
  received the sanitizer or coverage flags); the clang function sanitizer then had to be told
  about libev's callback dispatch (`EV_NO_SANITIZE_FUNCTION` on `ev_invoke` / `ev_invoke_pending`,
  which call a `void(*)(EV_P_ ev_timer*,int)` through the generic `ev_watcher*` type — the
  contract libev is built on), and `ev_wrap.h` is regenerated from `ev_vars.h` again
  (`sh ./update_ev_wrap`; hand edits since QB-188 had failed its own reproducibility check on
  every POSIX CI job).
- **Windows: the `QueryPerformanceCounter` clock of QB-193 was never compiled in (Huly
  QB-195).** libev's "fixes any misconfiguration" block reads `#ifndef CLOCK_MONOTONIC` and
  forces `EV_USE_MONOTONIC` to 0 — and MSVC has no `CLOCK_MONOTONIC` — 450 lines after QB-193 had
  set it to 1 for `_WIN32`. So `get_clock` was `ev_time`, the loop's "monotonic" time was the
  precise SYSTEM time (QB-193's other half), stepped by every wall-clock adjustment, and the
  QPC path was dead code in the standalone and in qb's copy alike; `test_clock_resolution` could
  not tell, a precise system clock also moving a thousand times in 50 ms without stepping back
  inside them. The first `ev_now_set` case did: `ev_clock_now()` read Unix seconds. The block
  now exempts Windows. Measured after: `ev_clock_now` reads the uptime scale, `ev_now` the
  interpolated realtime, and `bench-pass`'s `timer` shape 31.5 → 28.6 ns (QPC is the cheaper
  read). The figures QB-193 published (a pass 15.8 → 31 ns, a timer arm 5.8 → 24) were those of
  the precise system time; its precision claim held, its monotonicity claim did not until now.
- **The wepoll suite measured nothing: its five cases passed on `fd_kill`'s `EV_ERROR`, never
  through wepoll (Huly QB-194).** `tests/test-wepoll.c` wrote `ev_io_init(&w, cb, (int)sock,
  EV_READ)` — a raw winsock `SOCKET` as the fd, registered nowhere in the `SOCKET ↔ fd`
  registry — so `ev_io_start` resolved it to `INVALID_SOCKET`, wepoll's `EPOLL_CTL_ADD` failed,
  `fd_kill` stopped the watcher and fed it `EV_ERROR | EV_READ | EV_WRITE`, and callbacks that
  never looked at `revents` took the kill for a delivery (`recv()` on the raw value still
  worked: the kill leaves the socket itself intact). Measured on MSVC 19.51: `revents`
  0x80000003 in every case, the loop's io count 0 after the pass, the backend never consulted —
  including case 2, the `87bb2e03` regression the fork exists for. The sockets go through
  `ev_io_init_sock` / `ev_io_set_sock` now and callbacks read `w->handle`; every verdict
  requires `EV_ERROR` absent (a kill is a failure, never a delivery), case 1 also asserts the
  watcher is still active afterwards, case 2 covers both `ev_io_modify` in place and the
  stop / set / start cycle, and the unconditional floor is 5 → 13 with the new `EVRUN_NOPOLL`
  case (QB-191). Replanting the raw-`SOCKET` form is rejected (3 FAIL); the corrected suite
  13/13 ×10. Exposed by that sixth case, which asks whether a pass LOOKED and so counts what
  wepoll fed.

- **Windows: the loop's clocks are `QueryPerformanceCounter` and
  `GetSystemTimePreciseAsFileTime`, not the system tick (Huly QB-193).** `ev_win32.c` read
  the wall clock from `GetSystemTimeAsFileTime` — a memory read of the system TICK, stepping
  every 1 to 15.6 ms (2.2 ms measured on Windows 11) — and MSVC having no `clock_gettime`,
  `EV_USE_MONOTONIC` was 0 and that same tick was the MONOTONIC clock: every timer was judged
  at the tick's granularity (a 0-second timer started and run inside one tick did not fire; a
  1 ms timeout could fire 16 ms late), and a wall-clock adjustment moved the loop's idea of
  now. The monotonic clock is `QueryPerformanceCounter` now (sub-microsecond, never steps
  back, no syscall), the wall clock `GetSystemTimePreciseAsFileTime` (Windows 8+, resolved at
  run time with the coarse call as the fallback, read once per `MIN_TIMEJUMP/2` like every
  other host). Found by the loop suite the first time it ran on MSVC: `test_io_count`'s
  0-second timer did not fire on its NOWAIT pass, and the re-init of the still-active watcher
  corrupted the heap for the check after it. What it costs — the honest half: a precise read
  is ~16 ns where the tick was 3, so on MSVC a non-blocking pass is 15.8 → 31 ns, a timer arm
  with `ev_now_update` 5.8 → 24, and qb's timed ask 115 → 166 ns; the qev programme's next
  steps take the libev timer off the request path and hand the embedder's own reading to the
  loop (Huly QB-189, QB-190). `test_clock_resolution` pins it: `ev_now` moves at least 1000
  times in 50 ms of back-to-back reads and never steps back; the MSVC loop suite is
  **39 run / 0 failed / 3 skipped** (was 27 / 2 / 2).

- **A loop with no fd watcher no longer pays a backend poll it cannot use.** `ev_run` called
  `backend_poll` unconditionally, so an `EVRUN_NOWAIT` pass over a loop holding only timers —
  the shape an embedder's request timeout, retry or sleep leaves behind — cost a bare
  `epoll_wait(0)` / `kevent` / wepoll syscall on every pass for the life of the timer. Measured
  in qb 3.2's core on i9-12900K / WSL2 g++-14: a coroutine request/reply that arms a 500 ms
  timeout cost **~800 ns** per round trip against 46 ns without the timeout, three passes each
  paying the syscall. The loop now keeps a count of its active `ev_io` watchers (the timerfd
  and the signalfd included, since they are `ev_io`s on real fds; the loop's own evpipe is not
  — see the non-blocking pass entry above) and
  skips the poll when that count is zero AND the wait would not block anyway; a blocking wait
  is kept, because with no fd it is the sleep. Timers, periodics, idle/prepare/check and
  pending events are reified exactly as before — `tests/test-loops.c` (`test_io_count`) pins
  that a timers-only `EVRUN_NOWAIT` pass still fires an expired timer and that a timers-only
  blocking run still sleeps.

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

[Unreleased]: https://github.com/isndev/qev/compare/v5.2.0...HEAD
[5.2.0]: https://github.com/isndev/qev/compare/v5.1.0...v5.2.0
[5.1.0]: https://github.com/isndev/qev/compare/v5.0.0...v5.1.0
[5.0.0]: https://github.com/isndev/qev/releases/tag/v5.0.0
