# Programs.  Don't install testprog.
bin_PROGRAMS 	= osd_cat
noinst_PROGRAMS = testprog

osd_cat_SOURCES  = osd_cat.c
testprog_SOURCES = testprog.c

osd_cat_LDADD 	= libxosd/libxosd.la
testprog_LDADD 	= libxosd/libxosd.la

include_HEADERS = xosd.h

CFLAGS=@CFLAGS@ ${GTK_CFLAGS}

# Plugins (configure will determine what to build)
SUBDIRS=libxosd @SELECTED_PLUGINS@ 
