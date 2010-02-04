NULL =

INCLUDES = \
	-I. 					\
	-I$(top_srcdir)/common			\
	-I$(top_srcdir)/common/linux	\
	$(PROTOCOL_CFLAGS)			\
	$(FFMPEG_CFLAGS)			\
	$(QCAIRO_CFLAGS)			\
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
	$(top_srcdir)/common/gl_canvas.c		\
	$(top_srcdir)/common/region.c			\
	$(top_srcdir)/common/glc.c			\
	$(top_srcdir)/common/ogl_ctx.c			\
	$(top_srcdir)/common/rop3.c			\
	$(top_srcdir)/common/quic.c			\
	$(top_srcdir)/common/lz.c			\
	$(top_srcdir)/common/canvas_utils.c		\
	$(NULL)

lib_LTLIBRARIES = libspice.la

libspice_la_LDFLAGS = 				\
	-version-number $(SPICE_LT_VERSION)	\
	-no-undefined				\
	$(NULL)

libspice_la_LIBADD = 				\
	$(GL_LIBS)				\
	$(FFMPEG_LIBS)				\
	$(QCAIRO_LIBS)				\
	$(SSL_LIBS)				\
	$(CELT051_LIBS)				\
	$(SLIRP_LIBS)				\
	$(LIBRT)				\
	$(NULL)

libspice_la_SOURCES =				\
	glz_encoder.c				\
	glz_encoder_config.h			\
	glz_encoder_dictionary.c		\
	glz_encoder_dictionary.h		\
	glz_encoder_dictionary_protected.h	\
	glz_encoder.h				\
	red_bitmap_utils.h			\
	red_client_cache.h			\
	red_client_shared_cache.h		\
	red_common.h				\
	red_dispatcher.c			\
	red_dispatcher.h			\
	reds.c					\
	reds.h					\
	stat.h					\
	red_worker.c				\
	red_worker.h				\
	red_yuv.h				\
	snd_worker.c				\
	snd_worker.h				\
	red_channel.h				\
	red_channel.c				\
	red_tunnel_worker.c			\
	red_tunnel_worker.h			\
	spice.h					\
	vd_interface.h				\
	$(COMMON_SRCS)				\
	$(NULL)

libspiceincludedir = $(includedir)/libspice
libspiceinclude_HEADERS =               	\
	spice.h					\
	vd_interface.h				\
	$(NULL)

EXTRA_DIST =					\
	glz_encode_match_tmpl.c			\
	glz_encode_tmpl.c			\
	$(NULL)
