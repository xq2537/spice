SUBDIRS = common server client python_modules

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = spice-server.pc

DISTCLEANFILES =                                \
	spice-server.pc

EXTRA_DIST = spice.proto spice1.proto spice_codegen.py

DISTCHECK_CONFIGURE_FLAGS=--enable-opengl --enable-gui --enable-tunnel --enable-smartcard --with-sasl
