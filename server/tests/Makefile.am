NULL =

INCLUDES =                              \
	-I$(top_srcdir)/server          \
	-I$(top_srcdir)/common          \
	$(PROTOCOL_CFLAGS)              \
	$(SPICE_NONPKGCONFIG_CFLAGS)    \
	$(NULL)

AM_LDFLAGS = -L../.libs -lspice-server

noinst_PROGRAMS = test_just_sockets_no_ssl test_empty_success test_fail_on_null_core_interface test_display_no_ssl

test_display_no_ssl_SOURCES = test_display_no_ssl.c basic_event_loop.c basic_event_loop.h test_util.h

test_display_no_ssl_LDFLAGS = $(AM_LDFLAGS) $(LDFLAGS)

test_just_sockets_no_ssl_SOURCES = test_just_sockets_no_ssl.c basic_event_loop.c basic_event_loop.h test_util.h

test_just_sockets_no_ssl_LDFLAGS = $(AM_LDFLAGS) $(LDFLAGS)

test_empty_success_SOURCES = test_empty_success.c

test_empty_success_LDFLAGS = $(AM_LDFLAGS) $(LDFLAGS)

test_fail_on_null_core_interface_SOURCES = test_fail_on_null_core_interface.c

test_fail_on_null_core_interface_LDFLAGS = $(AM_LDFLAGS) $(LDFLAGS)


