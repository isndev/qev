AUTOMAKE_OPTIONS = foreign

# libtool interface version (current:revision:age). 5 is qev's own line, not libev's.
VERSION_INFO = 5:0:0

EXTRA_DIST = LICENSE THIRD-PARTY-NOTICES README.md CHANGELOG.md \
	     CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md qev.m4 autogen.sh \
	     ev_vars.h ev_wrap.h \
	     ev_epoll.c ev_select.c ev_poll.c ev_kqueue.c ev_port.c ev_linuxaio.c ev_iouring.c \
	     ev_win32.c wepoll.c wepoll.h \
	     docs/ev.pod Symbols.ev Symbols.event

man_MANS = qev.3

# Installed under $(includedir)/qev/, never loose at the include root -- the CMake build makes
# the same choice for the same reason (see CMakeLists.txt's header-namespace block).
# $(includedir)/qev -- the SAME path the CMake build installs to, so <qev/ev.h> resolves
# identically however the consumer obtained the library.
qevincludedir = $(includedir)/qev
qevinclude_HEADERS = ev.h ev++.h

# libqev.a, NEVER libev.a. The fork exports qev_* and cannot satisfy a -lev consumer; taking
# libev's artefact name is how an installed copy of this used to shadow a real libev.
lib_LTLIBRARIES = libqev.la

libqev_la_SOURCES = ev.c event.c
libqev_la_LDFLAGS = -version-info $(VERSION_INFO)

# qev.3 is generated from ev.pod (not shipped pre-built; regenerated at build time).
# qev.3, not ev.3: the page is an INSTALLED artefact and would otherwise overwrite a real
# libev's in a shared prefix. The .pod it is generated from keeps libev's own name.
qev.3: docs/ev.pod
	pod2man -n QEV -r "qev-$(VERSION)" -c "qev - a modernized libev fork" -s3 <$< >$@

CLEANFILES = qev.3
