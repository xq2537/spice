NULL =

COMMON_DIR=$(SPICE_COMMON_SRCDIR)

INCLUDES = \
	-I. 					\
	-I$(COMMON_DIR)			\
	$(FFMPEG_CFLAGS)			\
	$(QCAIRO_CFLAGS)			\
	$(GL_CFLAGS)				\
	$(LOG4CPP_CFLAGS)			\
	$(SSL_CFLAGS)				\
	$(CELT051_CFLAGS)			\
	-DCAIRO_CANVAS_IMAGE_CACHE		\
	-DRED_STATISTICS			\
	$(WARN_CFLAGS)                          \
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(NULL)

COMMON_SRCS = 				 	\
	$(COMMON_DIR)/cairo_canvas.c		\
	$(COMMON_DIR)/gl_canvas.c			\
	$(COMMON_DIR)/region.c			\
	$(COMMON_DIR)/glc.c				\
	$(COMMON_DIR)/ogl_ctx.c			\
	$(COMMON_DIR)/rop3.c			\
	$(COMMON_DIR)/quic.c			\
	$(COMMON_DIR)/lz.c				\
	$(COMMON_DIR)/canvas_utils.c		\
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
	spice.h					\
	vd_interface.h				\
	$(NULL)

nodist_libspice_la_SOURCES = $(COMMON_SRCS)
libspiceincludedir = $(includedir)/libspice
libspiceinclude_HEADERS =               	\
	spice.h					\
	vd_interface.h				\
	$(NULL)

EXTRA_DIST =					\
	glz_encode_match_tmpl.c			\
	glz_encode_tmpl.c			\
	ffmpeg_inc.h.in				\
	$(NULL)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = spice-server.pc

DISTCLEANFILES =                                \
	ffmpeg_inc.h				\
	spice-server.pc

dist-hook:
	cp -p -f ./autogen.sh ../configure.ac.shared $(distdir)
