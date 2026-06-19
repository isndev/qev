#!/usr/bin/env bash
# qb-ev exhaustive build matrix.
#
# Usage:
#   scripts/test-build-matrix.sh native      # build on the host (macOS/Linux)
#   scripts/test-build-matrix.sh gcc|clang   # inside an Ubuntu container (apt-install first)
#
# Exercises: CMake static + shared, autotools ./configure && make (libtool static
# + shared), find_package(libev) consumer (static & shared), ASan/UBSan smoke,
# per-backend runtime probe, and the C++ (ev++.h) wrapper.
#
# Emits "RESULT|<cell>|<PASS|FAIL|SKIP>|<detail>" lines and a final summary.
# Exits 0 by default (the caller tallies RESULT lines). Set STRICT=1 to exit
# non-zero when any cell FAILs (used by CI).
#
# Env: CC/CXX override the compiler. seccomp=unconfined is recommended in Docker
# so the io_uring backend is actually reachable.
set -u

MODE="${1:-native}"
SRC_RO="${SRC_RO:-/src}"
[ "$MODE" = native ] && SRC_RO="${SRC_RO_NATIVE:-$PWD}"
WORK="${WORK:-/work}"
[ "$MODE" = native ] && WORK="$(mktemp -d)"

PASS=0; FAIL=0; SKIP=0
res(){ printf 'RESULT|%s|%s|%s\n' "$1" "$2" "${3:-}"; case "$2" in PASS)PASS=$((PASS+1));; FAIL)FAIL=$((FAIL+1));; SKIP)SKIP=$((SKIP+1));; esac; }
have(){ command -v "$1" >/dev/null 2>&1; }

# ---- toolchain (container only) -------------------------------------------
if [ "$MODE" = gcc ] || [ "$MODE" = clang ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/tmp/apt.log 2>&1
  PKGS="cmake make pkg-config git ca-certificates autoconf automake libtool m4 perl libssl-dev zlib1g-dev"
  if [ "$MODE" = gcc ]; then PKGS="$PKGS gcc g++"; export CC=gcc CXX=g++; else PKGS="$PKGS clang"; export CC=clang CXX=clang++; fi
  apt-get install -y -qq $PKGS >>/tmp/apt.log 2>&1 || { echo "APT FAILED"; tail -20 /tmp/apt.log; }
fi
: "${CC:=cc}"; : "${CXX:=c++}"
echo "== qb-ev matrix mode=$MODE  CC=$CC ($($CC --version 2>/dev/null|head -1))  on $(uname -srm) =="

rm -rf "$WORK"; mkdir -p "$WORK"; cp -a "$SRC_RO"/. "$WORK"/ 2>/dev/null
cd "$WORK" || { echo "no workdir"; exit 0; }

# ---- smoke sources --------------------------------------------------------
cat > smoke.c <<'EOF'
#include <ev.h>
#include <stdio.h>
static int ticks=0;
static void tcb(struct ev_loop*l, ev_timer*w, int r){ (void)w;(void)r; if(++ticks>=3) ev_break(l, EVBREAK_ALL); }
int main(void){
  struct ev_loop*l=ev_default_loop(0);
  if(!l){ printf("NO DEFAULT LOOP\n"); return 1; }
  printf("default backend=0x%x ver=%d.%d\n", ev_backend(l), ev_version_major(), ev_version_minor());
  ev_timer t; ev_timer_init(&t,tcb,0.001,0.001); ev_timer_start(l,&t);
  ev_run(l,0);
  printf("ticks=%d %s\n", ticks, ticks>=3?"OK":"BAD");
  return ticks>=3?0:1;
}
EOF
cat > backend.c <<'EOF'
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){
  unsigned want=(unsigned)strtoul(argv[1],0,0);
  if(!(ev_supported_backends()&want)){ printf("UNSUP 0x%x\n",want); return 2; }
  struct ev_loop*l=ev_loop_new(want);
  if(!l){ printf("NULL 0x%x\n",want); return 2; }
  unsigned got=ev_backend(l);
  printf("want=0x%x got=0x%x %s\n",want,got,got==want?"MATCH":"FALLBACK");
  ev_loop_destroy(l);
  return got==want?0:3;
}
EOF
cat > smoke.cpp <<'EOF'
#include <ev++.h>
#include <cstdio>
int main(){ ev::default_loop loop; std::printf("cxx backend=0x%x ok\n", loop.backend()); return 0; }
EOF

# ---- ver check ------------------------------------------------------------
V=$(grep -m1 'EV_VERSION_MAJOR' ev.h | grep -o '[0-9]*')
[ "$V" = 5 ] && res ev.h-version PASS "major=$V" || res ev.h-version FAIL "major=$V"

# ---- cell: cmake static ---------------------------------------------------
if cmake -S . -B b-static -DCMAKE_BUILD_TYPE=Release >b-static-cfg.log 2>&1 \
   && cmake --build b-static -j >b-static.log 2>&1 && ls b-static/libev.a >/dev/null 2>&1; then
  W=$(grep -ci 'warning:' b-static.log)
  [ "$W" = 0 ] && res cmake-static PASS "libev.a, 0 warn" || res cmake-static FAIL "$W warnings"
else res cmake-static FAIL "build error: $(tail -3 b-static.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: cmake shared ---------------------------------------------------
if cmake -S . -B b-shared -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON >b-shared-cfg.log 2>&1 \
   && cmake --build b-shared -j >b-shared.log 2>&1; then
  SO=$(ls b-shared/libev.so* b-shared/libev*.dylib 2>/dev/null | head -1)
  W=$(grep -ci 'warning:' b-shared.log)
  if [ -n "$SO" ] && [ "$W" = 0 ]; then res cmake-shared PASS "$(basename "$SO"), 0 warn"; else res cmake-shared FAIL "so='$SO' warn=$W"; fi
else res cmake-shared FAIL "build error: $(tail -3 b-shared.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: cmake install + find_package(libev) consumer (static) ----------
cmake --install b-static --prefix "$WORK/pfx-st" >inst-st.log 2>&1
mkdir -p consumer && cat > consumer/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(c C)
find_package(libev 5 REQUIRED)
add_executable(c ../smoke.c)
target_link_libraries(c PRIVATE qb::ev)
EOF
if cmake -S consumer -B consumer/b -DCMAKE_PREFIX_PATH="$WORK/pfx-st" >con-st.log 2>&1 \
   && cmake --build consumer/b -j >>con-st.log 2>&1 && consumer/b/c >con-st-run.log 2>&1; then
  res findpackage-consumer-static PASS "$(head -1 con-st-run.log)"
else res findpackage-consumer-static FAIL "$(tail -3 con-st.log con-st-run.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: cmake install shared + consumer (links shared ev) --------------
cmake --install b-shared --prefix "$WORK/pfx-sh" >inst-sh.log 2>&1
if cmake -S consumer -B consumer/bsh -DCMAKE_PREFIX_PATH="$WORK/pfx-sh" >con-sh.log 2>&1 \
   && cmake --build consumer/bsh -j >>con-sh.log 2>&1; then
  if DYLD_LIBRARY_PATH="$WORK/pfx-sh/lib" LD_LIBRARY_PATH="$WORK/pfx-sh/lib" consumer/bsh/c >con-sh-run.log 2>&1; then
    res findpackage-consumer-shared PASS "$(head -1 con-sh-run.log)"
  else res findpackage-consumer-shared FAIL "run: $(tail -2 con-sh-run.log|tr '\n' ' ')"; fi
else res findpackage-consumer-shared FAIL "$(tail -3 con-sh.log|tr '\n' ' ')"; fi

# ---- cell: autotools configure + make (libtool: static + shared) ----------
if have autoreconf; then
  if autoreconf --install --force >auto-recon.log 2>&1 && ./configure >auto-cfg.log 2>&1 && make -j >auto-make.log 2>&1; then
    LA=$(ls libev.la 2>/dev/null); A=$(ls .libs/libev.a 2>/dev/null)
    S=$(ls .libs/libev.so* .libs/libev*.dylib 2>/dev/null|head -1)   # .so on Linux, .dylib on macOS
    res autotools-configure-make PASS "la=$([ -n "$LA" ]&&echo y) a=$([ -n "$A" ]&&echo y) so=$(basename "${S:-none}")"
    if [ -n "$S" ] && $CC smoke.c -I. -L.libs -lev -o smoke_auto >auto-link.log 2>&1 \
       && DYLD_LIBRARY_PATH=.libs LD_LIBRARY_PATH=.libs ./smoke_auto >auto-run.log 2>&1; then
      res autotools-shared-run PASS "$(head -1 auto-run.log)"
    else res autotools-shared-run FAIL "$(tail -2 auto-link.log auto-run.log 2>/dev/null|tr '\n' ' ')"; fi
  else res autotools-configure-make FAIL "$(grep -iE 'error|no such|cannot' auto-recon.log auto-cfg.log auto-make.log 2>/dev/null|head -3|tr '\n' ' ')"; fi
else res autotools-configure-make SKIP "no autoreconf on host"; res autotools-shared-run SKIP "no autotools"; fi

# ---- cell: ASan/UBSan smoke ----------------------------------------------
# clang's -fsanitize=undefined enables -fsanitize=function for C, which flags
# libev's intentional callback function-pointer type-pun (the common-initial-
# sequence idiom central to the ev_* API). Exclude it on clang ONLY; gcc neither
# supports nor enables the function sanitizer for C. All other checks stay on.
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -g"
case "$($CC --version 2>&1)" in *clang*) SAN="$SAN -fno-sanitize=function";; esac
if cmake -S . -B b-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="$SAN" >b-san-cfg.log 2>&1 \
   && cmake --build b-san -j >b-san.log 2>&1 \
   && $CC $SAN smoke.c -I. -Ib-san b-san/libev.a -lm -o smoke_san >san-link.log 2>&1; then
  LEAK=0; [ "$(uname -s)" = Linux ] && LEAK=1   # detect_leaks unsupported on macOS ASan
  if ASAN_OPTIONS=detect_leaks=$LEAK UBSAN_OPTIONS=halt_on_error=1 ./smoke_san >san-run.log 2>&1; then
    res asan-ubsan-smoke PASS "$(grep -m1 ticks san-run.log)"
  else res asan-ubsan-smoke FAIL "sanitizer: $(grep -iE 'runtime error|ERROR|leak' san-run.log|head -1)"; fi
else res asan-ubsan-smoke FAIL "build/link: $(tail -2 b-san.log san-link.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: per-backend smoke ---------------------------------------------
if $CC backend.c -I. -Ib-static b-static/libev.a -lm -o backendtest >be-link.log 2>&1; then
  detail=""; ok=1
  for be in 0x1:select 0x2:poll 0x4:epoll 0x8:kqueue 0x20:port 0x40:linuxaio 0x80:iouring; do
    code="${be%%:*}"; name="${be##*:}"
    out=$(./backendtest "$code" 2>&1); rc=$?
    case $rc in 0) detail="$detail $name=MATCH";; 2) detail="$detail $name=unsup";; *) detail="$detail $name=$out"; ok=0;; esac
  done
  [ "$ok" = 1 ] && res per-backend-smoke PASS "$detail" || res per-backend-smoke FAIL "$detail"
else res per-backend-smoke FAIL "link: $(tail -2 be-link.log|tr '\n' ' ')"; fi

# ---- cell: C++ ev++.h smoke ----------------------------------------------
if $CXX smoke.cpp -I. -Ib-static b-static/libev.a -lm -o smoke_cxx >cxx-link.log 2>&1 && ./smoke_cxx >cxx-run.log 2>&1; then
  res cxx-evpp-smoke PASS "$(head -1 cxx-run.log)"
else res cxx-evpp-smoke FAIL "$(tail -2 cxx-link.log cxx-run.log 2>/dev/null|tr '\n' ' ')"; fi

echo "== SUMMARY mode=$MODE: PASS=$PASS FAIL=$FAIL SKIP=$SKIP =="
if [ "${STRICT:-0}" = 1 ] && [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
