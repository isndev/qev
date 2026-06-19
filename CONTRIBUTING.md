# Contributing to qb-ev

Thanks for your interest in qb-ev! This is a hardened, modernized fork of
[libev](http://software.schmorp.de/pkg/libev.html) (with a bundled, patched
[wepoll](https://github.com/piscisaureus/wepoll) on Windows). The goal is a
**drop-in `ev_*` library that is correct, warning-clean, and fast on every
platform**.

## Ground rules

- **Do not break the public API/ABI.** The `ev_*` symbols, struct layouts and the
  `event.h` libevent compatibility shim are a contract. ABI-affecting changes need
  a very good reason and a version bump.
- **Keep it warning-clean.** The library builds under a strict
  `-Wall -Wextra -Wpedantic -Wshadow -Wcast-align …` set with **zero** warnings.
- **Preserve behaviour** except for proven bugs, and call out any behaviour change
  explicitly in the PR and `CHANGELOG.md`.
- **No mass reformatting.** Match the surrounding style (the core is vendored from
  libev and keeps its style).

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DBUILD_SHARED_LIBS=ON for a shared lib
cmake --build build
ctest --test-dir build --output-on-failure       # watcher coverage
./build/qb-ev-bench-backends                      # backend benchmark
```

Autotools is also supported:

```sh
./autogen.sh && ./configure && make
```

## Testing before you open a PR

Run the full build matrix (CMake static/shared, autotools, `find_package`
consumer, ASan/UBSan, every backend, C++ wrapper):

```sh
# Locally:
bash scripts/test-build-matrix.sh native

# In a clean Ubuntu container (also exercises autotools + io_uring):
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src:ro ubuntu:24.04 \
  bash -c 'bash /src/scripts/test-build-matrix.sh gcc'
```

CI (`.github/workflows/ci.yml`) runs the same matrix on Linux (gcc + clang) and
macOS, plus a Windows/MSVC build, on every push and PR.

## Submitting

1. Branch from `main`.
2. Make focused commits; explain *why*, not just *what*.
3. Add or update a watcher/backend test in `tests/` when fixing a bug.
4. Add an entry to `CHANGELOG.md` under the unreleased section.
5. Ensure `scripts/test-build-matrix.sh native` is green and the build is
   warning-free.
6. Open the PR using the template.

## Reporting bugs / security issues

Use the GitHub issue templates for bugs and feature requests. For **security**
vulnerabilities, follow [SECURITY.md](SECURITY.md) — do not open a public issue.

## License

By contributing, you agree that your contributions are licensed under the project's
[MIT license](LICENSE). Portions derived from libev and wepoll remain under their
original BSD-2-Clause terms (see [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES)).
