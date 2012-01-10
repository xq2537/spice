if OS_WIN32
SUBDIRS = win
endif

NULL =

noinst_LTLIBRARIES = libspice-common.la
libspice_common_la_SOURCES =		\
	bitops.h			\
	canvas_utils.c			\
	canvas_utils.h			\
	draw.h				\
	lines.c				\
	lines.h				\
	lz.c				\
	lz.h				\
	lz_common.h			\
	lz_config.h			\
	marshaller.c			\
	marshaller.h			\
	mem.c				\
	mem.h				\
	messages.h			\
	mutex.h				\
	pixman_utils.c			\
	pixman_utils.h			\
	quic.c				\
	quic.h				\
	quic_config.h			\
	rect.h				\
	region.c			\
	region.h			\
	ring.h				\
	rop3.c				\
	rop3.h				\
	spice_common.h			\
	ssl_verify.c			\
	ssl_verify.h			\
	backtrace.c			\
	backtrace.h			\
	$(NULL)

if SUPPORT_GL
libspice_common_la_SOURCES +=		\
	gl_utils.h			\
	glc.h				\
	glc.c				\
	ogl_ctx.h			\
	ogl_ctx.c			\
	$(NULL)
endif

INCLUDES = \
	$(GL_CFLAGS)			\
	$(PIXMAN_CFLAGS)		\
	$(PROTOCOL_CFLAGS)		\
	$(VISIBILITY_HIDDEN_CFLAGS)	\
	$(WARN_CFLAGS)			\
	-std=gnu99			\
	$(NULL)

EXTRA_DIST =				\
	canvas_base.c			\
	canvas_base.h			\
	gdi_canvas.c			\
	gdi_canvas.h			\
	gl_canvas.c			\
	gl_canvas.h			\
	sw_canvas.c			\
	sw_canvas.h			\
	lz_compress_tmpl.c		\
	lz_decompress_tmpl.c		\
	quic_family_tmpl.c		\
	quic_rgb_tmpl.c			\
	quic_tmpl.c			\
	$(NULL)
