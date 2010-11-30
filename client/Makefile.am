NULL =

SUBDIRS = . $(red_target) gui
DIST_SUBDIRS = x11 windows gui

spice_built_sources = generated_demarshallers.cpp generated_marshallers.cpp generated_demarshallers1.cpp generated_marshallers1.cpp

generated_demarshallers.cpp: $(top_srcdir)/spice.proto
	$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-demarshallers --client --include common.h --include messages.h $(top_srcdir)/spice.proto generated_demarshallers.cpp

generated_demarshallers1.cpp: $(top_srcdir)/spice1.proto
	$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-demarshallers --client --include common.h --include messages.h --prefix 1 --ptrsize 8 $(top_srcdir)/spice1.proto generated_demarshallers1.cpp

generated_marshallers.cpp: $(top_srcdir)/spice.proto
	$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-marshallers -P  --include "common.h" --include messages.h --include marshallers.h --client $(top_srcdir)/spice.proto generated_marshallers.cpp

generated_marshallers1.cpp: $(top_srcdir)/spice1.proto
	$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-marshallers -P  --include "common.h" --include messages.h --include marshallers.h --client --prefix 1 --ptrsize 8 $(top_srcdir)/spice1.proto generated_marshallers1.cpp

if SUPPORT_GL
GL_SRCS =				\
	gl_canvas.cpp			\
	glc.cpp				\
	red_gl_canvas.cpp		\
	red_gl_canvas.h			\
	red_pixmap_gl.h
else
GL_SRCS =
endif

SMARTCARD_SRC_ENABLED = smartcard_channel.cpp smartcard_channel.h

if SUPPORT_SMARTCARD
SMARTCARD_SRCS = $(SMARTCARD_SRC_ENABLED)
else
SMARTCARD_SRCS =
endif

RED_COMMON_SRCS =			\
	application.cpp			\
	application.h			\
	audio_channels.h		\
	audio_devices.h			\
	cache.hpp			\
	sw_canvas.cpp			\
	pixman_utils.cpp		\
	lines.cpp			\
	canvas.cpp			\
	canvas.h			\
	canvas_utils.cpp		\
	red_sw_canvas.cpp		\
	red_sw_canvas.h			\
	client_net_socket.cpp		\
	client_net_socket.h		\
	cmd_line_parser.cpp		\
	cmd_line_parser.h		\
	common.h			\
	demarshallers.h			\
	marshallers.h			\
	generated_demarshallers.cpp	\
	generated_demarshallers1.cpp	\
	marshaller.cpp			\
	generated_marshallers.cpp	\
	generated_marshallers1.cpp	\
	controller.cpp			\
	controller.h			\
	cursor_channel.cpp		\
	cursor_channel.h		\
	cursor.cpp			\
	cursor.h			\
	debug.h				\
	display_channel.cpp		\
	display_channel.h		\
	foreign_menu.cpp		\
	foreign_menu.h			\
	glz_decoded_image.h		\
	glz_decoder_config.h		\
	glz_decoder.cpp			\
	glz_decoder.h			\
	glz_decoder_window.cpp		\
	glz_decoder_window.h		\
	glz_decode_tmpl.c		\
	inputs_channel.cpp		\
	inputs_channel.h		\
	inputs_handler.h		\
	jpeg_decoder.cpp		\
	jpeg_decoder.h			\
	lz.cpp				\
	monitor.cpp			\
	monitor.h			\
	mem.cpp				\
	menu.cpp			\
	menu.h				\
	mjpeg_decoder.h			\
	mjpeg_decoder.cpp		\
	event_sources.h			\
	pixels_source.h			\
	platform.h			\
	playback_channel.cpp		\
	process_loop.cpp		\
	process_loop.h			\
	quic.cpp			\
	read_write_mutex.h		\
	record_channel.cpp		\
	red_channel.cpp			\
	red_channel.h			\
	red_client.cpp			\
	red_client.h			\
	red_drawable.h			\
	red_key.h			\
	red_peer.cpp			\
	red_peer.h			\
	red_pixmap_sw.h			\
	red_pixmap.h			\
	red_types.h			\
	red_window.h			\
	region.cpp			\
	rop3.cpp			\
	screen.cpp			\
	screen.h			\
	screen_layer.cpp		\
	screen_layer.h			\
	shared_cache.hpp		\
	tunnel_channel.cpp		\
	tunnel_channel.h		\
	hot_keys.cpp			\
	hot_keys.h			\
	threads.cpp			\
	threads.h			\
	utils.cpp			\
	utils.h				\
	zlib_decoder.cpp		\
	zlib_decoder.h			\
	$(NULL)

GDI_FILES =				\
	gdi_canvas.cpp			\
	red_gdi_canvas.cpp		\
	red_gdi_canvas.h		\
	red_pixmap_gdi.h		\
	$(NULL)

MAINTAINERCLEANFILES = $(spice_built_sources)

EXTRA_DIST = $(RED_COMMON_SRCS) $(spice_built_sources) $(GL_SRCS) $(GDI_FILES) $(SMARTCARD_SRC_ENABLED)

BUILT_SOURCES = $(spice_built_sources)
