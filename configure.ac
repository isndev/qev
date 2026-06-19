dnl qb-ev autotools build. Keep ev.h EV_VERSION_MAJOR/MINOR in sync with the
dnl version below.
AC_PREREQ([2.69])
AC_INIT([qb-ev],[5.0],[https://github.com/isndev/qb-ev/issues],[qb-ev],
        [https://github.com/isndev/qb-ev])

orig_CFLAGS="$CFLAGS"

AC_CONFIG_SRCDIR([ev_epoll.c])
AC_CONFIG_MACRO_DIRS([.])
AM_INIT_AUTOMAKE([foreign subdir-objects])

AC_CONFIG_HEADERS([config.h])
AM_MAINTAINER_MODE

AC_PROG_CC
AM_PROG_AR

dnl Supply default CFLAGS, if not specified
if test -z "$orig_CFLAGS"; then
  if test x$GCC = xyes; then
    CFLAGS="-g -O3"
  fi
fi

AC_PROG_INSTALL
LT_INIT

m4_include([libev.m4])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
