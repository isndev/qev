AUTOMAKE_OPTIONS = foreign

# libtool interface version (current:revision:age). Bumped to 5 for qb-ev.
VERSION_INFO = 5:0:0

EXTRA_DIST = LICENSE THIRD-PARTY-NOTICES README.md CHANGELOG.md \
	     CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md libev.m4 autogen.sh \
	     ev_vars.h ev_wrap.h \
	     ev_epoll.c ev_select.c ev_poll.c ev_kqueue.c ev_port.c ev_linuxaio.c ev_iouring.c \
	     ev_win32.c wepoll.c wepoll.h \
	     ev.pod Symbols.ev Symbols.event

man_MANS = ev.3

include_HEADERS = ev.h ev++.h event.h event_compat.h

# Drop-in compatible: the installed library is still named libev.
lib_LTLIBRARIES = libev.la

libev_la_SOURCES = ev.c event.c
libev_la_LDFLAGS = -version-info $(VERSION_INFO)

# ev.3 is generated from ev.pod (not shipped pre-built; regenerated at build time).
ev.3: ev.pod
	pod2man -n LIBEV -r "qb-ev-$(VERSION)" -c "qb-ev - a modernized libev fork" -s3 <$< >$@

CLEANFILES = ev.3
