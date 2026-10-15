/*
 * qev — white-box test of the Windows process-wide state (Windows only).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * The SOCKET <-> fd registry and the clock caches of ev_win32.c are process-wide and private: no
 * public call reaches their allocation failures, nor the moment several threads first touch them.
 * So this test compiles ev.c INTO itself -- it links no qev, whose symbols it would duplicate --
 * with the CRT allocator and two Win32 calls replaced by test doubles defined before the include,
 * and it reads the static state directly. Nothing here exists in a library build.
 *
 *   1. The clock caches (Huly QB-337). N threads read both clocks cold, at once. The caches must
 *      be written by ONE thread while the others wait for it -- before, every cold thread wrote
 *      them, unsynchronised. The doubles of GetProcAddress and QueryPerformanceFrequency are the
 *      initialisation itself: each holds its window open until every racing thread is inside it,
 *      or for a second, so an unsynchronised initialisation is seen with all N threads in it,
 *      deterministically, and a serialised one with exactly one. It must run first: a process has
 *      ONE cold start.
 *   2. The registry's free list (Huly QB-315). Its growth used to be announced before the
 *      reallocation and its failure ignored: a release under a failing allocator left a capacity
 *      with no storage behind it (NULL on the first growth, the old block on a later one) and the
 *      next release wrote through it. The oracle is the heap itself: every capacity the registry
 *      announces must fit the block it points to (_msize). Then an acquire under a failing
 *      allocator: -1 / ENOMEM, and nothing consumed.
 *
 * Registered by CMake only on WIN32.
 */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

/* ---- test doubles: defined BEFORE ev.c, so their own calls reach the real functions ---- */

static volatile LONG fail_realloc, fail_calloc; /* while set, every such allocation fails */
static void *test_realloc(void *p, size_t n) { return fail_realloc && n ? NULL : realloc(p, n); }
static void *test_calloc(size_t c, size_t n) { return fail_calloc ? NULL : calloc(c, n); }

#define CLOCK_THREADS 4
typedef struct {
    volatile LONG inside, peak, calls;
} clk_window_t;
static clk_window_t clk_gpa, clk_qpf;
static void clk_hold(clk_window_t *w) {
    LONG      now = InterlockedIncrement(&w->inside), peak;
    ULONGLONG t0  = GetTickCount64();
    InterlockedIncrement(&w->calls);
    while ((peak = w->peak) < now && InterlockedCompareExchange(&w->peak, now, peak) != peak) {}
    while (w->peak < CLOCK_THREADS && GetTickCount64() - t0 < 1000) Sleep(1);
    InterlockedDecrement(&w->inside);
}
static FARPROC WINAPI test_GetProcAddress(HMODULE m, LPCSTR name) {
    if (((ULONG_PTR) name >> 16) && !strcmp(name, "GetSystemTimePreciseAsFileTime"))
        clk_hold(&clk_gpa);
    return GetProcAddress(m, name);
}
static BOOL WINAPI test_QueryPerformanceFrequency(LARGE_INTEGER *f) {
    clk_hold(&clk_qpf);
    return QueryPerformanceFrequency(f);
}

#define realloc test_realloc
#define calloc test_calloc
#define GetProcAddress test_GetProcAddress
#define QueryPerformanceFrequency test_QueryPerformanceFrequency
#include "../ev.c"
#undef realloc
#undef calloc
#undef GetProcAddress
#undef QueryPerformanceFrequency

static int g_fail = 0, g_run = 0, g_skip = 0;
#define OK(cond, name)   do { ++g_run; if (cond) printf("  PASS  %s\n", (name)); else { printf("  FAIL  %s\n", (name)); ++g_fail; } } while (0)
#define SKIP(name, why)  do { ++g_skip; printf("  SKIP  %s (%s)\n", (name), (why)); } while (0)

/* ---- 1. the clock caches, cold, from N threads at once (QB-337) ---- */
static HANDLE    clk_gate;
static ev_tstamp clk_wall[CLOCK_THREADS], clk_mono[CLOCK_THREADS];
static DWORD WINAPI clk_thread(LPVOID arg) {
    int i = (int) (intptr_t) arg;
    WaitForSingleObject(clk_gate, INFINITE);
    clk_wall[i] = ev_time();
    clk_mono[i] = ev_win32_get_clock();
    return 0;
}
static void test_clock_cold_start(void) {
    HANDLE th[CLOCK_THREADS];
    int    i, started = 0, sane = 1;
    clk_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < CLOCK_THREADS; ++i)
        if ((th[started] = CreateThread(NULL, 0, clk_thread, (LPVOID) (intptr_t) i, 0, NULL)) != NULL) ++started;
    SetEvent(clk_gate);
    for (i = 0; i < started; ++i) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
    CloseHandle(clk_gate);
    if (started < CLOCK_THREADS) { SKIP("the clock caches, cold, from N threads", "CreateThread failed"); return; }
    for (i = 0; i < CLOCK_THREADS; ++i)
        sane = sane && clk_wall[i] > 1.6e9 && clk_mono[i] > 0.;
    printf("        GetSystemTimePreciseAsFileTime resolved %ld time(s), at most %ld at once; QPF scale computed %ld time(s), at most %ld at once\n",
           clk_gpa.calls, clk_gpa.peak, clk_qpf.calls, clk_qpf.peak);
    OK(sane, "four threads reading both clocks cold, at once, all read a Unix-epoch wall time and a positive monotonic time");
    OK(clk_gpa.calls == 1 && clk_gpa.peak == 1,
       "the precise wall clock is resolved by ONE thread while the others wait for it, never by several at once (QB-337)");
    OK(clk_qpf.calls == 1 && clk_qpf.peak == 1,
       "the QueryPerformanceCounter scale is computed by ONE thread while the others wait for it, never by several at once (QB-337)");
}

/* ---- 2. the SOCKET <-> fd registry under allocation failure (QB-315) ---- */

/* Handle values the registry only stores and hashes: no socket is created, none is closed. */
static SOCKET fake(int i) { return (SOCKET) (0x10000 + 4 * (uintptr_t) i); }

/* Every capacity the registry announces is backed by the block it points to, and the free list
   holds no more than it announces. */
static int registry_sound(void) {
    if (ev_win32_fd_freecnt < 0 || ev_win32_fd_freecnt > ev_win32_fd_freemax)
        return 0;
    if (ev_win32_fd_freemax && (!ev_win32_fd_free || _msize(ev_win32_fd_free) < (size_t) ev_win32_fd_freemax * sizeof(int)))
        return 0;
    if (ev_win32_fd_by_fdmax && (!ev_win32_fd_by_fd || _msize(ev_win32_fd_by_fd) < (size_t) ev_win32_fd_by_fdmax * sizeof(*ev_win32_fd_by_fd)))
        return 0;
    return 1;
}
static void registry_dump(void) {
    printf("        free list: %d recorded, %d announced, %ld bytes behind it; by_fd: %d announced, %ld bytes behind it; next index %d\n",
           ev_win32_fd_freecnt, ev_win32_fd_freemax, ev_win32_fd_free ? (long) _msize(ev_win32_fd_free) : 0L,
           ev_win32_fd_by_fdmax, ev_win32_fd_by_fd ? (long) _msize(ev_win32_fd_by_fd) : 0L, ev_win32_fd_next);
}

/* Back to an empty registry between scenarios, so each one starts from the state it names. Only
   called on a sound registry: the blocks freed are the ones the registry really owns. */
static void registry_reset(void) {
    unsigned int b;
    ev_win32_fd_init(); /* the bucket table exists from the registry's first use on */
    for (b = 0; b < EV_WIN32_FD_BUCKETS; ++b)
        while (ev_win32_fd_buckets[b]) {
            ev_win32_fd_entry *e = ev_win32_fd_buckets[b];
            ev_win32_fd_buckets[b] = e->next;
            free(e);
        }
    free(ev_win32_fd_by_fd);
    free(ev_win32_fd_free);
    ev_win32_fd_by_fd    = 0;
    ev_win32_fd_free     = 0;
    ev_win32_fd_by_fdmax = ev_win32_fd_freemax = ev_win32_fd_freecnt = ev_win32_fd_next = 0;
}

#define NREG 100

static void test_registry_first_growth(void) {
    int fd0, fd1, r;
    registry_reset();
    fd0          = ev_win32_fd_acquire(fake(1), 0);
    fail_realloc = 1;
    r            = ev_win32_fd_release(fd0, 0); /* the first index ever returned */
    fail_realloc = 0;
    registry_dump();
    OK(fd0 >= 0 && r == 0 && registry_sound(),
       "a release under a failing allocator, the free list never grown yet: every capacity the registry announces is backed by storage (QB-315)");
    if (!registry_sound()) {
        SKIP("the index released under a failing allocator is reused in bounds", "the registry is unsound: the next release would write through it");
        return;
    }
    fd1 = ev_win32_fd_acquire(fake(2), 0);
    r   = ev_win32_fd_release(fd1, 0);
    OK(fd1 >= 0 && r == 0 && registry_sound(), "the next acquire and release stay in bounds");
}

static void test_registry_later_growth(void) {
    int fd[NREG], i, r, all = 1;
    registry_reset();
    for (i = 0; i < NREG; ++i) all = all && (fd[i] = ev_win32_fd_acquire(fake(100 + i), 0)) >= 0;
    for (i = 0; i < 64; ++i) all = all && ev_win32_fd_release(fd[i], 0) == 0; /* 64 indices recorded */
    fail_realloc = 1;
    r            = ev_win32_fd_release(fd[64], 0); /* the 65th: past the first block */
    fail_realloc = 0;
    registry_dump();
    OK(all && r == 0 && registry_sound(),
       "a release under a failing allocator past the free list's first block: every capacity the registry announces is backed by storage (QB-315)");
    if (!registry_sound()) {
        SKIP("the remaining releases and a full re-acquire stay in bounds", "the registry is unsound: the next release would write past its block");
        return;
    }
    for (i = 65; i < NREG; ++i) all = all && ev_win32_fd_release(fd[i], 0) == 0;
    {
        int next0 = ev_win32_fd_next, again = 1;
        for (i = 0; i < NREG; ++i) again = again && (fd[i] = ev_win32_fd_acquire(fake(300 + i), 0)) >= 0 && fd[i] < next0;
        OK(all && again && ev_win32_fd_next == next0 && registry_sound(),
           "every released index is recorded and handed out again, in bounds, without issuing a new one");
        for (i = 0; i < NREG; ++i) ev_win32_fd_release(fd[i], 0);
    }
}

static void test_registry_acquire_fails(void) {
    int i = 0, next0, r, err, fd;
    registry_reset();
    /* acquire until handing out a NEW index needs the tables to grow */
    do fd = ev_win32_fd_acquire(fake(500 + i++), 0);
    while (fd >= 0 && !(ev_win32_fd_freecnt == 0 && ev_win32_fd_next >= ev_win32_fd_by_fdmax) && i < 10000);
    next0        = ev_win32_fd_next;
    fail_realloc = 1;
    r            = ev_win32_fd_acquire(fake(9999), 0);
    err          = errno;
    fail_realloc = 0;
    EnterCriticalSection(&ev_win32_fd_lock);
    fd = ev_win32_fd_find_handle_unlocked(fake(9999)) != 0;
    LeaveCriticalSection(&ev_win32_fd_lock);
    OK(r == -1 && err == ENOMEM && !fd && ev_win32_fd_next == next0 && registry_sound(),
       "an acquire whose tables cannot grow fails with ENOMEM and changes nothing: no entry, no index consumed (QB-315)");
    r = ev_win32_fd_acquire(fake(9999), 0);
    OK(r == next0 && registry_sound(), "the same acquire succeeds once memory is back, with the index the failed one left unconsumed");

    fail_calloc = 1; /* the entry itself cannot be allocated; the tables need not grow */
    fd          = ev_win32_fd_acquire(fake(10000), 0);
    err         = errno;
    fail_calloc = 0;
    r           = ev_win32_fd_acquire(fake(10000), 0);
    OK(fd == -1 && err == ENOMEM && r >= 0 && registry_sound(),
       "control: an acquire whose entry cannot be allocated fails with ENOMEM, returns its index, and the retry succeeds");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* a red run on an unsound registry may die: keep every line printed so far */
    printf("qev Windows internals (white-box, ev %d.%d)\n", ev_version_major(), ev_version_minor());

    test_clock_cold_start(); /* first: the clock caches have one cold start per process */
    test_registry_first_growth();
    test_registry_later_growth();
    test_registry_acquire_fails();

    printf("\n== win32-internals: %d run, %d failed, %d skipped ==\n", g_run, g_fail, g_skip);

    /* Ten checks are unconditional (clock 3, first growth 2, later growth 2, acquire 3): a SKIP
       above follows a FAIL, or a thread that could not be created. A passing run below ten
       measured less than it claims. */
    if (!g_fail && g_run < 10) {
        printf("== FAIL: only %d checks ran; 10 are unconditional ==\n", g_run);
        return 1;
    }
    return g_fail ? 1 : 0;
}

#else /* !_WIN32 */
/* Never registered by CMake off Windows; this stub only keeps a stray compile honest. */
#include <stdio.h>
int main(void) {
    printf("the Windows internals test is Windows-only; this binary should not have been built here\n");
    return 1;
}
#endif
