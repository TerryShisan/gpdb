#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/ipcclean/Makefile,v 1.22 2008/01/01 19:45:55 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/ipcclean
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: ipcclean

ipcclean: ipcclean.sh
	cp -f $< $@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) ipcclean '$(DESTDIR)$(bindir)/ipcclean'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/ipcclean'

clean distclean maintainer-clean:
	rm -f ipcclean
