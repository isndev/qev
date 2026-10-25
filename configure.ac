dnl qev autotools build. Keep ev.h EV_VERSION_MAJOR/MINOR in sync with the
dnl version below.
AC_PREREQ([2.69])
AC_INIT([qev],[5.2.0],[https://github.com/isndev/qev/issues],[qev],
        [https://github.com/isndev/qev])

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

AC_ARG_ENABLE([libevent-compat],
  [AS_HELP_STRING([--enable-libevent-compat], [build the unprefixed event_* compatibility API (default: no)])],
  [], [enable_libevent_compat=no])
AS_CASE([$enable_libevent_compat], [yes|no], [],
  [AC_MSG_ERROR([--enable-libevent-compat accepts yes or no])])
AM_CONDITIONAL([LIBEVENT_COMPAT], [test "x$enable_libevent_compat" = xyes])

m4_include([qev.m4])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
