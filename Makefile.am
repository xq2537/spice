NULL =
ACLOCAL_AMFLAGS = -I m4

SUBDIRS = spice-common server

if SUPPORT_CLIENT
SUBDIRS += client
endif

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = spice-server.pc

DISTCHECK_CONFIGURE_FLAGS =			\
	--enable-automated-tests		\
	--enable-opengl				\
	--enable-smartcard			\
	--with-sasl				\
	$(NULL)
