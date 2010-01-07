NULL =

COMMON_SRCS = 				\
	cairo_canvas.h			\
	cairo_canvas.c			\
	canvas_base.h			\
	canvas_base.c			\
	canvas_utils.h			\
	canvas_utils.c			\
	draw.h				\
	gl_canvas.h			\
	gl_canvas.c			\
	glc.h				\
	glc.c				\
	gl_utils.h			\
	lookup3.h			\
	lookup3.c			\
	lz_common.h			\
	mutex.h				\
	ogl_ctx.h			\
	ogl_ctx.c			\
	quic.h				\
	quic.c				\
	quic_config.h			\
	qxl_dev.h			\
	rect.h				\
	red_error_codes.h		\
	red.h				\
	reds_stat.h			\
	region.h			\
	region.c			\
	ring.h				\
	rop3.h				\
	rop3.c				\
	lz.c				\
	lz_compress_tmpl.c		\
	lz_config.h			\
	lz_decompress_tmpl.c		\
	lz.h				\
	quic_family_tmpl.c		\
	quic_rgb_tmpl.c			\
	quic_tmpl.c			\
	ipc_ring.h			\
	vd_agent.h			\
	quic_tmpl.c			\
	$(NULL)

EXTRA_DIST = $(COMMON_SRCS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = spice-common.pc

common_srcdir=$(prefix)/src/spice-common
common_src_DATA = $(COMMON_SRCS)

DISTCLEANFILES = spice-common.pc

dist-hook:
	cp -p -f ../autogen.sh ../configure.ac.shared $(distdir)

