
bin_PROGRAMS += src/bin/examine

src_bin_examine_SOURCES = src/bin/examine_main.c

src_bin_examine_CPPFLAGS = \
-I$(top_srcdir)/src/lib \
-DPACKAGE_BIN_DIR=\"$(bindir)\" \
@EXM_CPPFLAGS@

src_bin_examine_CFLAGS = @EXM_CFLAGS@

src_bin_examine_LDADD = \
src/lib/libexamine.la
