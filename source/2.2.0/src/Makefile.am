# Programs.  Don't install testprog.
bin_PROGRAMS 	= osd_cat
noinst_PROGRAMS = testprog

osd_cat_SOURCES  = osd_cat.c
testprog_SOURCES = testprog.c

osd_cat_LDADD 	= libxosd/libxosd.la
testprog_LDADD 	= libxosd/libxosd.la

include_HEADERS = xosd.h

AM_CFLAGS=${GTK_CFLAGS}

# Plugins (configure will determine what to build)
SUBDIRS=libxosd @SELECTED_PLUGINS@ 
DIST_SUBDIRS=libxosd xmms_plugin xmms_plugin_oldplugin
