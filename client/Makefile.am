NULL =

SUBDIRS = . $(red_target)
DIST_SUBDIRS = x11 #windows

spice_built_sources = generated_demarshallers.cpp

generated_demarshallers.cpp: $(top_srcdir)/spice.proto
	$(PYTHON) $(top_srcdir)/spice_codegen.py --generate-demarshallers --client --include common.h $(top_srcdir)/spice.proto generated_demarshallers.cpp

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
	cursor_channel.cpp		\
	cursor_channel.h		\
	cursor.cpp			\
	cursor.h			\
	debug.h				\
	display_channel.cpp		\
	display_channel.h		\
	red_gl_canvas.cpp		\
	red_gl_canvas.h			\
	gl_canvas.cpp			\
	glc.cpp				\
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
	pixels_source.h			\
	platform.h			\
	playback_channel.cpp		\
	process_loop.cpp		\
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
	red_pixmap_gl.h			\
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
	$(NULL)

MAINTAINERCLEANFILES = $(spice_built_sources)

EXTRA_DIST = $(RED_COMMON_SRCS) $(spice_built_sources)

BUILT_SOURCES = $(spice_built_sources)
