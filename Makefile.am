ACLOCAL_AMFLAGS = -I m4

CONF_FILE = cliap2.conf

BUILT_SOURCES = $(CONF_FILE)

SUBDIRS = src

EXTRA_DIST = \
    $(CONF_FILE).in

CLEANFILES = $(BUILT_SOURCES)

do_subst = $(SED) -e 's|@sbindir[@]|$(sbindir)|g' \
             -e 's|@localstatedir[@]|$(localstatedir)|g' \
             -e 's|@PACKAGE[@]|$(PACKAGE)|g' \
             -e 's|@OWNTONE_USER[@]|$(OWNTONE_USER)|g'

# these files use $prefix, which is determined at build (not configure) time
$(CONF_FILE): Makefile
	$(AM_V_at)rm -f $@ $@-t
	$(AM_V_GEN)$(do_subst) "$(srcdir)/$@.in" > $@-t
	$(AM_V_at)mv $@-t $@

$(CONF_FILE): $(srcdir)/$(CONF_FILE).in
