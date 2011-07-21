SUBDIRS = . tests

NULL =

INCLUDES = \
	-I.					\
	-I$(top_srcdir)/common			\
	-I$(top_srcdir)/common/linux	\
	$(PROTOCOL_CFLAGS)			\
	$(JPEG_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(GL_CFLAGS)				\
	$(SSL_CFLAGS)				\
	$(SASL_CFLAGS)				\
	$(CELT051_CFLAGS)			\
	$(SLIRP_CFLAGS)				\
	-DSW_CANVAS_IMAGE_CACHE			\
	-DRED_STATISTICS			\
	$(WARN_CFLAGS)                          \
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(SMARTCARD_CFLAGS)				\
	$(NULL)

spice_built_sources = generated_marshallers.c generated_marshallers.h generated_demarshallers.c

generated_demarshallers.c: $(top_srcdir)/spice.proto
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-demarshallers --server --include red_common.h $(top_srcdir)/spice.proto generated_demarshallers.c

STRUCTS=-M String -M Rect -M Point -M DisplayBase -M Fill -M Opaque -M Copy -M Blend -M Blackness -M Whiteness -M Invers -M Rop3 -M Stroke -M Text -M Transparent -M AlphaBlend
generated_marshallers.c: $(top_srcdir)/spice.proto
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/spice_codegen.py --include red_common.h --generate-marshallers $(STRUCTS) --server $(top_srcdir)/spice.proto generated_marshallers.c

generated_marshallers.h: $(top_srcdir)/spice.proto
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-marshallers $(STRUCTS) --server -H $(top_srcdir)/spice.proto generated_marshallers.h

if SUPPORT_GL
GL_SRCS =				\
	$(top_srcdir)/common/gl_canvas.c		\
	$(top_srcdir)/common/glc.c			\
	$(top_srcdir)/common/ogl_ctx.c			\
	$(NULL)
else
GL_SRCS =
endif

COMMON_SRCS =						\
	$(top_srcdir)/common/sw_canvas.c		\
	$(top_srcdir)/common/pixman_utils.c		\
	$(top_srcdir)/common/lines.c			\
	$(top_srcdir)/common/region.c			\
	$(top_srcdir)/common/rop3.c			\
	$(top_srcdir)/common/quic.c			\
	$(top_srcdir)/common/lz.c			\
	$(top_srcdir)/common/canvas_utils.c		\
	$(top_srcdir)/common/mem.c			\
	$(top_srcdir)/common/marshaller.c		\
	$(NULL)

lib_LTLIBRARIES = libspice-server.la

libspice_server_la_LDFLAGS =			\
	-version-number $(SPICE_LT_VERSION)	\
	-Wl,--version-script=$(top_srcdir)/server/spice-server.syms	\
	-no-undefined				\
	$(NULL)

libspice_server_la_LIBADD =			\
	$(GL_LIBS)				\
	$(JPEG_LIBS)				\
	$(PIXMAN_LIBS)				\
	$(SSL_LIBS)				\
	$(SASL_LIBS)				\
	$(CELT051_LIBS)				\
	$(SLIRP_LIBS)				\
	$(LIBRT)				\
	$(Z_LIBS)				\
	$(NULL)

if SUPPORT_TUNNEL
TUNNEL_SRCS =			\
	red_tunnel_worker.c	\
	red_tunnel_worker.h	\
	$(NULL)
else
TUNNEL_SRCS =
endif

if SUPPORT_SMARTCARD
SMARTCARD_SRCS =		\
	smartcard.c			\
	smartcard.h			\
	$(NULL)
else
SMARTCARD_SRCS =
endif

libspice_server_la_SOURCES =			\
	agent-msg-filter.c			\
	agent-msg-filter.h			\
	demarshallers.h				\
	glz_encoder.c				\
	glz_encoder_config.h			\
	glz_encoder_dictionary.c		\
	glz_encoder_dictionary.h		\
	glz_encoder_dictionary_protected.h	\
	glz_encoder.h				\
	jpeg_encoder.c				\
	jpeg_encoder.h				\
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
	red_parse_qxl.c				\
	red_parse_qxl.h				\
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
	spice-experimental.h			\
	generated_demarshallers.c		\
	generated_marshallers.c			\
	generated_marshallers.h			\
	zlib_encoder.c				\
	zlib_encoder.h				\
	char_device.h				\
	$(TUNNEL_SRCS)				\
	$(SMARTCARD_SRCS)			\
	$(COMMON_SRCS)				\
	$(GL_SRCS)				\
	$(NULL)

libspice_serverincludedir = $(includedir)/spice-server
libspice_serverinclude_HEADERS =		\
	spice.h					\
	spice-experimental.h			\
	$(NULL)

EXTRA_DIST =					\
	glz_encode_match_tmpl.c			\
	glz_encode_tmpl.c			\
	spice-server.syms			\
	$(NULL)

BUILT_SOURCES = $(spice_built_sources)
