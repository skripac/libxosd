%define plugindir	%(xmms-config --general-plugin-dir)
%define	aclocaldir	%(aclocal --print-ac-dir)

Name:		xosd
Version:	2.0.1
Release:	1
Vendor:		Andre Renaud <andre@ignavus.net>
Copyright:	GPL
Group:		System Environment/Libraries
Packager:	Oron Peled <oron@actcom.co.il>
Summary:	X On-Screen Display libray
Source:		%name-2.0.1.tar.gz

# Oron Peled - Remove after adding to the distribution
Source1:	xosd-config.in
Source2:	xosd-Makefile.am
Source3:	xosd-configure.in

URL:		http://www.ignavus.net/software.html
Buildroot:	%{_tmppath}/%{name}-%{version}-root

%description
X On-Screen Display library. A library for displaying a TV-like on-screen
display in X.

%package devel
Summary: X On-Screen Display libray (headers + static libs)
Group: Development/Libraries
Requires: xosd = %{version}

%description devel
On-Screen Display library. A library for displaying a TV-like on-screen
display in X.

%package xmms
Summary: X On-Screen Display libray (XMMS Plugin)
Group: Applications/Multimedia
Requires: xosd = %{version}, xmms

%description xmms
A plugin for XMMS

%prep
%setup -n %{name}-2.0.1

# Oron Peled - should be part of the main distribution
cp $RPM_SOURCE_DIR/libxosd.m4 .
cp $RPM_SOURCE_DIR/xosd-Makefile.am Makefile.am
cp $RPM_SOURCE_DIR/xosd-configure.in configure.in
cp $RPM_SOURCE_DIR/xosd-config.in script/xosd-config.in

%build
# Oron Peled - can be removed after fixes to distro
autoconf
automake

./configure --prefix=%{_prefix} --mandir=%{_mandir}
make

%install
[ "$RPM_BUILD_ROOT" != "/" ] && [ -d $RPM_BUILD_ROOT ] && rm -rf $RPM_BUILD_ROOT
make DESTDIR=$RPM_BUILD_ROOT install

%post
ldconfig

%postun
ldconfig

%files
%defattr(-, root, root)
%{_libdir}/libxosd.so.2.0.1
%{_libdir}/libxosd.so.2
%{_libdir}/libxosd.so
%{_bindir}/osd_cat
%doc %{_mandir}/man1/osd_cat.1*
%doc %{_mandir}/man3/xosd.3*
%doc AUTHORS ChangeLog COPYING NEWS README

%files devel
%defattr(-, root, root)
%{_libdir}/libxosd.a
%{_libdir}/libxosd.la
%{_includedir}/xosd.h
%{_bindir}/xosd-config
%{aclocaldir}/libxosd.m4
%doc %{_mandir}/man1/xosd-config.1*

%files xmms
%defattr(-, root, root)
%{plugindir}/libxmms_osd.so

%clean
[ "$RPM_BUILD_ROOT" != "/" ] && [ -d $RPM_BUILD_ROOT ] && rm -rf $RPM_BUILD_ROOT
