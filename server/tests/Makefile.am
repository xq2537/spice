NULL =

INCLUDES =                              \
	-I$(top_srcdir)                 \
	-I$(top_srcdir)/common          \
	-I$(top_srcdir)/server          \
	-I$(top_srcdir)/server/tests    \
	$(PROTOCOL_CFLAGS)              \
	$(SPICE_NONPKGCONFIG_CFLAGS)    \
	$(NULL)

AM_LDFLAGS = $(top_builddir)/server/libspice-server.la

COMMON_BASE=basic_event_loop.c basic_event_loop.h test_util.h ../../common/backtrace.c


noinst_PROGRAMS = test_just_sockets_no_ssl test_empty_success test_fail_on_null_core_interface test_display_no_ssl test_display_streaming test_playback

test_display_streaming_SOURCES = test_display_streaming.c test_display_base.c test_display_base.h $(COMMON_BASE)

test_display_no_ssl_SOURCES = test_display_no_ssl.c test_display_base.c test_display_base.h $(COMMON_BASE)

test_just_sockets_no_ssl_SOURCES = test_just_sockets_no_ssl.c $(COMMON_BASE)

test_empty_success_SOURCES = test_empty_success.c

test_fail_on_null_core_interface_SOURCES = test_fail_on_null_core_interface.c

test_playback_SOURCES = test_playback.c $(COMMON_BASE)
