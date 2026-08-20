# Contributing to qev

qev is a maintained continuation of [libev](http://software.schmorp.de/pkg/libev.html),
with a patched [wepoll](https://github.com/piscisaureus/wepoll) vendored for Windows. The
goal is an event loop that is **correct, warning-clean and fast on every platform**, while
its C API stays exactly libev's.

## Ground rules

- **The public API is a contract.** `ev_*` symbols, struct layouts and the `event.h`
  libevent shim are what consumers compiled against. A change that affects them needs a
  very good reason, a `CHANGELOG` entry that says so plainly, and a version bump.
- **Keep it warning-clean.** The tree builds under a strict
  `-Wall -Wextra -Wpedantic -Wshadow -Wcast-align …` set with **zero** warnings, and
  passes ASan/UBSan. A PR that adds one is not ready.
- **Preserve behaviour** except for proven bugs, and call out any behaviour change
  explicitly — in the PR *and* in `CHANGELOG.md`.
- **No mass reformatting.** The core is libev's, and it keeps libev's style. That is not
  taste: it is what makes a file-to-file comparison against upstream possible when a
  future change has to be read or picked up. A reformat destroys that permanently.
- **The source layout is upstream's, deliberately.** Everything flat, backends included
  from `ev.c` as one translation unit. The public/internal split a reader wants is
  delivered by the *install* — `include/qev/ev.h` ships, `ev_vars.h` does not.

## qev and qb — what a contributor needs to know

The [qb Actor Framework](https://github.com/isndev/qb) embeds this exact source and ships
it as `libqb-ev.a`. Two consequences you will run into:

1. **The same files exist in both repositories and must stay identical.** A change here
   that does not also land in qb's tree will be caught — qb's development superproject
   runs a byte-identity check over the shared files on every verification pass. Land both
   sides, or the next person inherits a divergence.
2. **qb builds a reduced profile.** Seven watcher families (idle, prepare, check, fork,
   child, async, embed) are compiled out there, so `struct ev_loop` has a different layout
   under qb than in a standalone build. If your change touches anything gated on
   `EV_*_ENABLE`, test both: `-DQB_EV_WATCHERS_FULL=ON` and `=OFF`. The matrix's
   `reduced-profile` cell does this on every run, so a change that only compiles under
   the full profile is caught rather than discovered by qb.

Fixes flow both ways. A backend bug surfaced by qb's test suite belongs here; an
improvement here reaches qb on its next sync.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DBUILD_SHARED_LIBS=ON for a shared lib
cmake --build build
ctest --test-dir build --output-on-failure       # watcher + loop coverage (+ wepoll on Windows)
./build/qev-bench-backends                       # backend benchmark
```

autotools is maintained alongside:

```sh
./autogen.sh && ./configure && make
```

## Testing before you open a PR

Run the full matrix — CMake static and shared, autotools through `make install`, a
`find_package(qev)` consumer in both link modes, ASan/UBSan, every backend, the C++
wrapper, the exported-symbol census, the shared-prefix install check, the reduced
watcher profile qb ships, and a regeneration of `ev_wrap.h`:

```sh
bash scripts/test-build-matrix.sh native
```

In a clean container, which also exercises autotools and `io_uring`:

```sh
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src:ro ubuntu:24.04 \
  bash -c 'bash /src/scripts/test-build-matrix.sh gcc'
```

CI runs the same matrix on Linux (gcc + clang) and macOS, and on Windows/MSVC builds,
tests and censuses the archive — every push and PR.

**A test that cannot fail is not a test.** When you fix a bug, add the case that
reproduces it and confirm it fails against the unfixed code before you commit the fix.

## Submitting

1. Branch from `develop`. (`main` is the released line.)
2. Focused commits, explaining *why* rather than *what*.
3. A test in `tests/` for any bug fixed.
4. An entry in `CHANGELOG.md` under the unreleased section.
5. `bash scripts/test-build-matrix.sh native` green, build warning-free.
6. Open the PR using the template.

## Reporting bugs and security issues

Use the GitHub issue templates for bugs and feature requests. For **security**
vulnerabilities follow [SECURITY.md](SECURITY.md) — do not open a public issue.

## License

By contributing you agree that your contributions are licensed under the project's
[MIT license](LICENSE). Portions derived from libev and wepoll remain under their original
BSD-2-Clause terms (see [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES)).
