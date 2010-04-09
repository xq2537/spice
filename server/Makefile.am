NULL =

INCLUDES = \
	-I. 					\
	-I$(top_srcdir)/common			\
	-I$(top_srcdir)/common/linux	\
	$(PROTOCOL_CFLAGS)			\
	$(JPEG_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(GL_CFLAGS)				\
	$(LOG4CPP_CFLAGS)			\
	$(SSL_CFLAGS)				\
	$(CELT051_CFLAGS)			\
	$(SLIRP_CFLAGS)				\
	-DCAIRO_CANVAS_IMAGE_CACHE		\
	-DRED_STATISTICS			\
	$(WARN_CFLAGS)                          \
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(NULL)

COMMON_SRCS = 						\
	$(top_srcdir)/common/cairo_canvas.c		\
	$(top_srcdir)/common/pixman_utils.c		\
	$(top_srcdir)/common/lines.c			\
	$(top_srcdir)/common/gl_canvas.c		\
	$(top_srcdir)/common/region.c			\
	$(top_srcdir)/common/glc.c			\
	$(top_srcdir)/common/ogl_ctx.c			\
	$(top_srcdir)/common/rop3.c			\
	$(top_srcdir)/common/quic.c			\
	$(top_srcdir)/common/lz.c			\
	$(top_srcdir)/common/canvas_utils.c		\
	$(top_srcdir)/common/mem.c			\
	$(NULL)

lib_LTLIBRARIES = libspice-server.la

libspice_server_la_LDFLAGS =			\
	-version-number $(SPICE_LT_VERSION)	\
	-no-undefined				\
	$(NULL)

libspice_server_la_LIBADD = 			\
	$(GL_LIBS)				\
	$(JPEG_LIBS)				\
	$(PIXMAN_LIBS)				\
	$(SSL_LIBS)				\
	$(CELT051_LIBS)				\
	$(SLIRP_LIBS)				\
	$(LIBRT)				\
	$(NULL)

if SUPPORT_TUNNEL
	TUNNEL_SRCS =			\
		red_tunnel_worker.c	\
		red_tunnel_worker.h	\
		$(NULL)
else
	TUNNEL_SRCS =
endif

libspice_server_la_SOURCES =			\
	glz_encoder.c				\
	glz_encoder_config.h			\
	glz_encoder_dictionary.c		\
	glz_encoder_dictionary.h		\
	glz_encoder_dictionary_protected.h	\
	glz_encoder.h				\
	mjpeg_encoder.h				\
	mjpeg_encoder.c				\
	red_bitmap_utils.h			\
	red_client_cache.h			\
	red_client_shared_cache.h		\
	red_common.h				\
	red_dispatcher.c			\
	red_dispatcher.h			\
	red_memslots.c				\
	red_memslots.h				\
	reds.c					\
	reds.h					\
	stat.h					\
	red_worker.c				\
	red_worker.h				\
	snd_worker.c				\
	snd_worker.h				\
	red_channel.h				\
	red_channel.c				\
	spice.h					\
	vd_interface.h				\
	$(TUNNEL_SRCS)				\
	$(COMMON_SRCS)				\
	$(NULL)

libspice_serverincludedir = $(includedir)/spice-server
libspice_serverinclude_HEADERS =               	\
	spice.h					\
	vd_interface.h				\
	$(NULL)

EXTRA_DIST =					\
	glz_encode_match_tmpl.c			\
	glz_encode_tmpl.c			\
	$(NULL)
