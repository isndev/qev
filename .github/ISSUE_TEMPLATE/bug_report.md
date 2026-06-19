---
name: Bug report
about: Report a problem in qb-ev
title: "[bug] "
labels: bug
---

**Describe the bug**
A clear description of what is wrong.

**Environment**
- qb-ev version / commit:
- OS + version:
- Compiler + version:
- Backend (epoll / kqueue / io_uring / linuxaio / port / wepoll / select / poll):
- Build system (CMake / autotools), static or shared:

**Reproducer**
Minimal code or steps that trigger the bug. A small `.c` using `ev_*` is ideal.

```c
// minimal reproducer
```

**Expected vs actual behaviour**

**Extra**
Stack trace, ASan/UBSan output, or `ev_loop_new(EVBACKEND_*)` + `ev_backend()`
values if relevant.
