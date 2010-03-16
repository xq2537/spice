NULL =

SUBDIRS = $(red_target)
DIST_SUBDIRS = x11 #windows

RED_COMMON_SRCS =	 		\
	application.cpp			\
	application.h			\
	audio_channels.h		\
	audio_devices.h			\
	cache.hpp			\
	cairo_canvas.cpp		\
	canvas.cpp			\
	canvas.h			\
	canvas_utils.cpp		\
	red_cairo_canvas.cpp		\
	red_cairo_canvas.h		\
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
	foreign_menu.cpp		\
	foreign_menu.h			\
	foreign_menu_prot.h		\
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
	lz.cpp				\
	monitor.cpp			\
	monitor.h			\
	menu.cpp			\
	menu.h				\
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
	red_pixmap_cairo.h		\
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
	hot_keys.cpp			\
	hot_keys.h			\
        threads.cpp			\
        threads.h			\
	utils.cpp			\
	utils.h				\
	icon.h				\
	event_sources.h			\
	red_gdi_canvas.cpp		\
	red_gdi_canvas.h		\
	red_pixmap_gdi.h		\
	gdi_canvas.cpp			\
	$(NULL)

EXTRA_DIST = $(RED_COMMON_SRCS) \
	x11/ffmpeg_inc.h.in

DISTCLEANFILES = x11/ffmpeg_inc.h

dist-hook:
	cp -p -f ./autogen.sh ../configure.ac.shared $(distdir)
