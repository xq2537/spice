SUBDIRS = . tests

NULL =

INCLUDES = \
	-I$(top_srcdir)				\
	-I$(top_srcdir)/common			\
	-DRED_STATISTICS			\
	$(CELT051_CFLAGS)			\
	$(PIXMAN_CFLAGS)			\
	$(PROTOCOL_CFLAGS)			\
	$(SASL_CFLAGS)				\
	$(SLIRP_CFLAGS)				\
	$(SMARTCARD_CFLAGS)			\
	$(SSL_CFLAGS)				\
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(WARN_CFLAGS)				\
	$(NULL)

spice_built_sources = generated_marshallers.c generated_marshallers.h generated_demarshallers.c

generated_demarshallers.c: $(top_srcdir)/spice.proto
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-demarshallers --server --include red_common.h $(top_srcdir)/spice.proto generated_demarshallers.c

STRUCTS=-M String -M Rect -M Point -M DisplayBase -M Fill -M Opaque -M Copy -M Blend -M Blackness -M Whiteness -M Invers -M Rop3 -M Stroke -M Text -M Transparent -M AlphaBlend
generated_marshallers.c: $(top_srcdir)/spice.proto
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/spice_codegen.py --include red_common.h --generate-marshallers $(STRUCTS) --server $(top_srcdir)/spice.proto generated_marshallers.c

generated_marshallers.h: $(top_srcdir)/spice.proto
	$(AM_V_GEN)$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-marshallers $(STRUCTS) --server -H $(top_srcdir)/spice.proto generated_marshallers.h

lib_LTLIBRARIES = libspice-server.la

libspice_server_la_LDFLAGS =						\
	-version-number $(SPICE_LT_VERSION)				\
	-Wl,--version-script=$(top_srcdir)/server/spice-server.syms	\
	-no-undefined							\
	$(NULL)

libspice_server_la_LIBADD =				\
	$(top_builddir)/common/libspice-common.la	\
	$(CELT051_LIBS)					\
	$(GL_LIBS)					\
	$(JPEG_LIBS)					\
	$(LIBRT)					\
	$(PIXMAN_LIBS)					\
	$(SASL_LIBS)					\
	$(SLIRP_LIBS)					\
	$(SSL_LIBS)					\
	$(Z_LIBS)					\
	$(NULL)

libspice_server_la_SOURCES =			\
	agent-msg-filter.c			\
	agent-msg-filter.h			\
	char_device.h				\
	demarshallers.h				\
	generated_demarshallers.c		\
	generated_marshallers.c			\
	generated_marshallers.h			\
	glz_encoder.c				\
	glz_encoder.h				\
	glz_encoder_config.h			\
	glz_encoder_dictionary.c		\
	glz_encoder_dictionary.h		\
	glz_encoder_dictionary_protected.h	\
	inputs_channel.c			\
	inputs_channel.h			\
	jpeg_encoder.c				\
	jpeg_encoder.h				\
	main_channel.c				\
	main_channel.h				\
	mjpeg_encoder.c				\
	mjpeg_encoder.h				\
	red_bitmap_utils.h			\
	red_channel.c				\
	red_channel.h				\
	red_client_cache.h			\
	red_client_shared_cache.h		\
	red_common.h				\
	dispatcher.c				\
	dispatcher.h				\
	red_dispatcher.c			\
	red_dispatcher.h			\
	main_dispatcher.c			\
	main_dispatcher.h			\
	red_memslots.c				\
	red_memslots.h				\
	red_parse_qxl.c				\
	red_parse_qxl.h				\
	red_worker.c				\
	red_worker.h				\
	reds.c					\
	reds.h					\
	reds_sw_canvas.c			\
	reds_sw_canvas.h			\
	snd_worker.c				\
	snd_worker.h				\
	spice-experimental.h			\
	spice.h					\
	stat.h					\
	spicevmc.c				\
	zlib_encoder.c				\
	zlib_encoder.h				\
	$(NULL)

if SUPPORT_TUNNEL
libspice_server_la_SOURCES +=	\
	red_tunnel_worker.c	\
	red_tunnel_worker.h	\
	$(NULL)
endif

if SUPPORT_GL
libspice_server_la_SOURCES +=	\
	reds_gl_canvas.c	\
	reds_gl_canvas.h	\
	$(NULL)
endif

if SUPPORT_SMARTCARD
libspice_server_la_SOURCES +=	\
	smartcard.c		\
	smartcard.h		\
	$(NULL)
endif

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
