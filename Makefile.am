AUTOMAKE_OPTIONS = foreign

# libtool interface version (current:revision:age). 5 is qev's own line, not libev's.
VERSION_INFO = 5:0:0

EXTRA_DIST = README.md CHANGELOG.md \
	     CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md qev.m4 autogen.sh \
	     ev_vars.h ev_wrap.h \
	     ev_epoll.c ev_select.c ev_poll.c ev_kqueue.c ev_port.c ev_linuxaio.c ev_iouring.c \
	     ev_win32.c wepoll.c wepoll.h \
	     docs/ev.pod Symbols.ev Symbols.event

man_MANS = qev.3

# Installed, not merely distributed. These two were in EXTRA_DIST, which puts them in the source
# tarball and nowhere else -- so `make install` left a library derived from libev and wepoll on
# disk with no copyright notice beside it, which is what BSD-2 clause 2 asks for. dist_doc_DATA
# both distributes and installs, under $(docdir) = $(datarootdir)/doc/qev, the same path the
# CMake build uses.
dist_doc_DATA = LICENSE THIRD-PARTY-NOTICES

# $(includedir)/qev, never loose at the include root: the SAME path the CMake build installs to,
# so <qev/ev.h> resolves identically however the consumer obtained the library. A prefix carrying
# a loose ev.h is how an installed copy of this used to shadow a real libev.
qevincludedir = $(includedir)/qev
qevinclude_HEADERS = ev.h ev++.h

# libqev.la, NEVER libev.la. The C API deliberately keeps libev's ev_* names, so the ARTEFACT
# name is the only thing separating the two on a shared prefix -- and -lev must keep meaning the
# real libev. (This comment used to read "the fork exports qev_*", which was true of a spelling
# that was tried and reverted; the conclusion was right and the reason was not.)
lib_LTLIBRARIES = libqev.la

libqev_la_SOURCES = ev.c event.c
libqev_la_LDFLAGS = -version-info $(VERSION_INFO)

# qev.3 is generated from ev.pod (not shipped pre-built; regenerated at build time).
# qev.3, not ev.3: the page is an INSTALLED artefact and would otherwise overwrite a real
# libev's in a shared prefix. The .pod it is generated from keeps libev's own name.
qev.3: docs/ev.pod
	pod2man -n QEV -r "qev-$(VERSION)" -c "qev - a modernized libev fork" -s3 <$< >$@

CLEANFILES = qev.3
