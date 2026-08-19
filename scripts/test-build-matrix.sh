#!/usr/bin/env bash
# qb-ev exhaustive build matrix.
#
# Usage:
#   scripts/test-build-matrix.sh native      # build on the host (macOS/Linux)
#   scripts/test-build-matrix.sh gcc|clang   # inside an Ubuntu container (apt-install first)
#
# Exercises: CMake static + shared, autotools ./configure && make (libtool static
# + shared), find_package(qev) consumer (static & shared), ASan/UBSan smoke,
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
#include <qev/ev.h>
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
#include <qev/ev.h>
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
// Constructing a loop proves the header parses and the archive links. It proves nothing about
// the watcher paths, and that gap is not hypothetical: `ev::io::set(int)` called ev_io_modify()
// where that name was only a MACRO writing w->events in memory, never telling the backend, and
// this cell watched it happen for two months without a word. So the wrapper is now DRIVEN --
// arm a watcher, retarget it with set(int) while it is ACTIVE (the exact path that broke), and
// require the callback to fire.
#include <qev/ev++.h>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <unistd.h>
#endif

static int fired = 0;

struct probe {
    ev::io   io;
    ev::timer bail;
    int      wfd;

    void on_io(ev::io &w, int revents) { fired = 1; w.stop(); bail.stop(); }
    void on_bail(ev::timer &, int)     { std::puts("cxx FAIL: watcher never fired"); std::exit(3); }
};

int main() {
    ev::default_loop loop;
    std::printf("cxx backend=0x%x\n", loop.backend());

    int fds[2];
#if defined(_WIN32)
    // Winsock has no pipe(), and wepoll only watches SOCKETs anyway -- which is the point on
    // this platform. Build a connected loopback pair by hand.
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::puts("cxx FAIL: WSAStartup"); return 2; }
    SOCKET lsn = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = static_cast<int>(sizeof a);
    if (lsn == INVALID_SOCKET || bind(lsn, (sockaddr *) &a, alen) != 0 || listen(lsn, 1) != 0 ||
        getsockname(lsn, (sockaddr *) &a, &alen) != 0) { std::puts("cxx FAIL: listen"); return 2; }
    SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli == INVALID_SOCKET || connect(cli, (sockaddr *) &a, alen) != 0) { std::puts("cxx FAIL: connect"); return 2; }
    SOCKET srv = accept(lsn, nullptr, nullptr);
    if (srv == INVALID_SOCKET) { std::puts("cxx FAIL: accept"); return 2; }
    closesocket(lsn);
    fds[0] = static_cast<int>(srv);   // read end  (watched)
    fds[1] = static_cast<int>(cli);   // write end (poked)
#else
    if (pipe(fds) != 0) { std::perror("pipe"); return 2; }
#endif

    probe p;
    p.wfd = fds[1];

    // Watch the WRITE end for READ: never ready, so nothing fires.
    p.io.set<probe, &probe::on_io>(&p);
    p.io.start(fds[1], ev::READ);

    // Flush that registration all the way into the backend FIRST. Without this the retarget
    // below is masked -- start() only MARKS the fd changed, and the pending fd_reify() would
    // read the already-updated mask and register the right thing even with the defect present.
    // Measured: the control planted the pre-e8090ecc body and this cell still passed until the
    // pump was added, i.e. it was testing nothing.
    loop.run(EVRUN_NOWAIT);

    // Now retarget a watcher the backend has already registered. This is the path that broke:
    // set(int) must tell the backend, not just rewrite w->events.
    p.io.set(ev::WRITE);

    p.bail.set<probe, &probe::on_bail>(&p);
    p.bail.start(2.0);

    loop.run();
    if (!fired) { std::puts("cxx FAIL: loop returned without firing"); return 4; }
    std::puts("cxx io::set(int) retarget ok");
    return 0;
}
EOF

# ---- ver check ------------------------------------------------------------
# Six places carry this number by hand, and qb has been bitten three times by exactly that shape:
# a version literal that stopped agreeing with its source and nothing looked. There is no single
# source of truth to derive from here (autotools and CMake each want their own), so the next best
# thing is to make disagreement loud.
V=$(grep -m1 'EV_VERSION_MAJOR' ev.h | grep -o '[0-9]*')
Vm=$(grep -m1 'EV_VERSION_MINOR' ev.h | grep -o '[0-9]*')
# NOT `grep 'VERSION [0-9]'` -- that matches cmake_minimum_required(VERSION 3.22) first and
# yields an empty string, which then 'disagrees' with everything. The project() version is the
# one on its own indented line.
CMV=$(grep -m1 -E '^[[:space:]]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
ACV=$(grep -m1 'AC_INIT' configure.ac | grep -oE '\[[0-9]+\.[0-9]+\]' | tr -d '[]')
LTV=$(grep -m1 'VERSION_INFO *=' Makefile.am | grep -oE '[0-9]+:[0-9]+:[0-9]+')
CLV=$(grep -m1 '^## \[' CHANGELOG.md | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
RDV=$(grep -m1 -oE 'version-[0-9]+\.[0-9]+' README.md | cut -d- -f2)
detail="ev.h=$V.$Vm cmake=$CMV autoconf=$ACV libtool=$LTV changelog=$CLV readme=$RDV"
bad=""
[ "$V" = 5 ] || bad="$bad ev.h-major"
[ "$CMV" = "$V.$Vm.0" ] || bad="$bad cmake"
[ "$ACV" = "$V.$Vm" ]   || bad="$bad autoconf"
[ "$LTV" = "$V:$Vm:0" ] || bad="$bad libtool"
[ "$CLV" = "$V.$Vm.0" ] || bad="$bad changelog"
[ "$RDV" = "$V.$Vm" ]   || bad="$bad readme"
[ -z "$bad" ] && res version-surface PASS "$detail" \
               || res version-surface FAIL "disagree:$bad ($detail)"

# ---- cell: cmake static ---------------------------------------------------
if cmake -S . -B b-static -DCMAKE_BUILD_TYPE=Release >b-static-cfg.log 2>&1 \
   && cmake --build b-static -j >b-static.log 2>&1 && ls b-static/libqev.a >/dev/null 2>&1; then
  W=$(grep -ci 'warning:' b-static.log)
  [ "$W" = 0 ] && res cmake-static PASS "libqev.a, 0 warn" || res cmake-static FAIL "$W warnings"
else res cmake-static FAIL "build error: $(tail -3 b-static.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: cmake shared ---------------------------------------------------
if cmake -S . -B b-shared -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON >b-shared-cfg.log 2>&1 \
   && cmake --build b-shared -j >b-shared.log 2>&1; then
  SO=$(ls b-shared/libqev.so* b-shared/libqev*.dylib 2>/dev/null | head -1)
  W=$(grep -ci 'warning:' b-shared.log)
  if [ -n "$SO" ] && [ "$W" = 0 ]; then res cmake-shared PASS "$(basename "$SO"), 0 warn"; else res cmake-shared FAIL "so='$SO' warn=$W"; fi
else res cmake-shared FAIL "build error: $(tail -3 b-shared.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: cmake install + find_package(qev) consumer (static) ------------
cmake --install b-static --prefix "$WORK/pfx-st" >inst-st.log 2>&1
mkdir -p consumer && cat > consumer/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(c C)
find_package(qev 5 REQUIRED)
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
  if autoreconf --install --force >auto-recon.log 2>&1 && ./configure --prefix="$WORK/pfx-auto" >auto-cfg.log 2>&1 && make -j >auto-make.log 2>&1; then
    LA=$(ls libqev.la 2>/dev/null); A=$(ls .libs/libqev.a 2>/dev/null)
    S=$(ls .libs/libqev.so* .libs/libqev*.dylib 2>/dev/null|head -1)   # .so on Linux, .dylib on macOS
    res autotools-configure-make PASS "la=$([ -n "$LA" ]&&echo y) a=$([ -n "$A" ]&&echo y) so=$(basename "${S:-none}")"
    # Compile against an INSTALLED autotools prefix rather than the build tree. The headers ship
    # at $(includedir)/qev/ now -- never loose at the include root -- so the build tree cannot
    # satisfy <qev/ev.h>, and testing the layout a packager actually gets is the better answer
    # anyway. `make install` also exercises the qevinclude_HEADERS rule itself.
    make install >auto-inst.log 2>&1
    APFX="$WORK/pfx-auto"
    if [ -n "$S" ] && $CC smoke.c -I"$APFX/include" -L"$APFX/lib" -lqev -o smoke_auto >auto-link.log 2>&1 \
       && DYLD_LIBRARY_PATH="$APFX/lib" LD_LIBRARY_PATH="$APFX/lib" ./smoke_auto >auto-run.log 2>&1; then
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
   && SANLIB=$(ls b-san/libqev*.a 2>/dev/null | head -1) \
   && [ -n "$SANLIB" ] \
   && $CC $SAN smoke.c -Ib-san/include -Ib-san "$SANLIB" -lm -o smoke_san >san-link.log 2>&1; then
  LEAK=0; [ "$(uname -s)" = Linux ] && LEAK=1   # detect_leaks unsupported on macOS ASan
  if ASAN_OPTIONS=detect_leaks=$LEAK UBSAN_OPTIONS=halt_on_error=1 ./smoke_san >san-run.log 2>&1; then
    res asan-ubsan-smoke PASS "$(grep -m1 ticks san-run.log)"
  else res asan-ubsan-smoke FAIL "sanitizer: $(grep -iE 'runtime error|ERROR|leak' san-run.log|head -1)"; fi
else res asan-ubsan-smoke FAIL "build/link: $(tail -2 b-san.log san-link.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: per-backend smoke ---------------------------------------------
if $CC backend.c -Ib-static/include -Ib-static b-static/libqev.a -lm -o backendtest >be-link.log 2>&1; then
  detail=""; ok=1
  for be in 0x1:select 0x2:poll 0x4:epoll 0x8:kqueue 0x20:port 0x40:linuxaio 0x80:iouring; do
    code="${be%%:*}"; name="${be##*:}"
    out=$(./backendtest "$code" 2>&1); rc=$?
    case $rc in 0) detail="$detail $name=MATCH";; 2) detail="$detail $name=unsup";; *) detail="$detail $name=$out"; ok=0;; esac
  done
  [ "$ok" = 1 ] && res per-backend-smoke PASS "$detail" || res per-backend-smoke FAIL "$detail"
else res per-backend-smoke FAIL "link: $(tail -2 be-link.log|tr '\n' ' ')"; fi

# ---- cell: C++ ev++.h wrapper --------------------------------------------
if $CXX smoke.cpp -Ib-static/include -Ib-static b-static/libqev.a -lm -o smoke_cxx >cxx-link.log 2>&1 && ./smoke_cxx >cxx-run.log 2>&1; then
  res cxx-qevpp-drive PASS "$(tail -1 cxx-run.log)"
else res cxx-qevpp-drive FAIL "$(tail -2 cxx-link.log cxx-run.log 2>/dev/null|tr '\n' ' ')"; fi

# ---- cell: shared-prefix install ------------------------------------------
# qb embeds this loop and installs it too, so both products can land in one prefix. They must not
# share a single path, and the reason is not tidiness: qb compiles the REDUCED watcher profile and
# the standalone compiles all fourteen, so `ev_config.h` differs and with it the layout of
# `struct ev_loop` (1224 bytes against 1520). Same-named files with different content is exactly
# the collision this fork exists to close. Measured here rather than trusted:
#   qb           -> lib/libqb-ev.a  + include/qb/ev/
#   standalone   -> lib/libqev.a    + include/qev/  + lib/cmake/qev/ + lib/pkgconfig/qev.pc
PFXQ="$WORK/pfx-shared"
cmake --install b-static --prefix "$PFXQ" >shared-inst.log 2>&1
# stand in for a qb install: the same two paths qb would write
mkdir -p "$PFXQ/include/qb/ev" "$PFXQ/lib"
: > "$PFXQ/lib/libqb-ev.a"; : > "$PFXQ/include/qb/ev/ev.h"
CLASH=""
for p in lib/libqev.a include/qev/ev.h include/qev/ev_config.h; do
  [ -s "$PFXQ/$p" ] || CLASH="$CLASH $p(missing-or-emptied)"
done
[ -s "$PFXQ/lib/libqb-ev.a" ] && CLASH="$CLASH libqb-ev.a(overwritten-by-standalone)"
[ -s "$PFXQ/include/qb/ev/ev.h" ] && CLASH="$CLASH qb/ev/ev.h(overwritten-by-standalone)"
if [ -z "$CLASH" ]; then
  res shared-prefix-install PASS "libqev.a + include/qev/ coexist with qb's libqb-ev.a + include/qb/ev/"
else res shared-prefix-install FAIL "path(s) collide:$CLASH"; fi

# ---- cell: exported-symbol census -----------------------------------------
# The invariant, and it needs an ARCHIVE so it cannot live in a source-level guard.
#   * ev_*      EXPECTED. The fork keeps libev's own API names so a migrating consumer's sources
#               compile unchanged; only the header PATH and the artefact name are ours.
#   * qev_*     FORBIDDEN. That spelling was tried and abandoned; source says ev, artefact says qev,
#               and one file carrying both is how this tree lost three days.
#   * event_*   FORBIDDEN unless QB_EV_LIBEVENT_COMPAT is ON. Those 24 unprefixed names collide with
#               a real libevent, which is far more widely deployed than libev, and the two declare
#               the same names over incompatible struct layouts.
if command -v nm >/dev/null 2>&1 && [ -f b-static/libqev.a ]; then
  SYMS=$(nm -g b-static/libqev.a 2>/dev/null | grep -E ' [TDS] ' | awk '{print $3}' | sed 's/^_//' | sort -u)
  NEV=$(printf '%s\n' "$SYMS" | grep -c '^ev_' || true)
  NQEV=$(printf '%s\n' "$SYMS" | grep -c '^qev_' || true)
  NEVT=$(printf '%s\n' "$SYMS" | grep -c '^event' || true)
  OTHER=$(printf '%s\n' "$SYMS" | grep -vE '^ev_|^event|^$' || true)
  NO=$(printf '%s' "$OTHER" | grep -c . || true)
  if   [ "$NEV" -lt 40 ]; then res symbol-census FAIL "only $NEV ev_* symbols — the archive is not what it should be"
  elif [ "$NQEV" -ne 0 ]; then res symbol-census FAIL "$NQEV qev_* symbol(s) — the abandoned spelling is back"
  elif [ "$NEVT" -ne 0 ]; then res symbol-census FAIL "$NEVT unprefixed event_* — the libevent shim must stay OFF"
  elif [ "$NO"  -ne 0 ]; then res symbol-census FAIL "$NO symbol(s) outside ev_*: $(printf '%s' "$OTHER" | tr '\n' ' ' | cut -c1-120)"
  else res symbol-census PASS "$NEV ev_*, 0 qev_*, 0 unprefixed event_*"
  fi
else res symbol-census SKIP "no nm, or no static archive"; fi

echo "== SUMMARY mode=$MODE: PASS=$PASS FAIL=$FAIL SKIP=$SKIP =="
if [ "${STRICT:-0}" = 1 ] && [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
