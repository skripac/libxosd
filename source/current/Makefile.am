if BUILD_NEW_PLUGIN
	NEW_SUB = pixmaps
endif
if BUILD_BEEP_MEDIA_PLUGIN
	NEW_SUB = pixmaps
endif
SUBDIRS = macros man $(NEW_SUB) src script
DIST_SUBDIRS = macros man pixmaps src script

DISTCLEANFILES = libtool-disable-static

aclocaldir	= $(datadir)/aclocal
aclocal_DATA	= libxosd.m4

EXTRA_DIST = autogen.sh libxosd.m4 xosd.spec xosd.spec.in
