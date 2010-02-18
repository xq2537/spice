NULL =

SUBDIRS = images

INCLUDES = \
	-DCAIRO_CANVAS_ACCESS_TEST			\
	-DCAIRO_CANVAS_CACHE				\
	-DCAIRO_CANVAS_NO_CHUNKS			\
	-DUSE_GLZ					\
	-DUSE_OGL					\
	-I. 						\
	-I..						\
	-I$(top_srcdir)/common				\
	-I$(top_srcdir)/common/linux		\
	-I$(top_srcdir)/client				\
	$(PROTOCOL_CFLAGS)				\
	$(GL_CFLAGS)					\
	$(ALSA_CFLAGS)					\
	$(FFMPEG_CFLAGS)				\
	$(QCAIRO_CFLAGS)				\
	$(LOG4CPP_CFLAGS)				\
	$(CELT051_CFLAGS)				\
	$(SSL_CFLAGS)					\
	$(XRANDR_CFLAGS)				\
	$(CEGUI_CFLAGS)					\
	$(WARN_CFLAGS)                                  \
	$(NULL)


RED_COMMON_SRCS = 					\
	$(top_srcdir)/client/application.cpp		\
	$(top_srcdir)/client/application.h		\
	$(top_srcdir)/client/audio_channels.h		\
	$(top_srcdir)/client/audio_devices.h		\
	$(top_srcdir)/client/cache.hpp			\
	$(top_srcdir)/client/cairo_canvas.cpp		\
	$(top_srcdir)/client/canvas.cpp			\
	$(top_srcdir)/client/canvas.h			\
	$(top_srcdir)/client/canvas_utils.cpp		\
	$(top_srcdir)/client/red_cairo_canvas.cpp	\
	$(top_srcdir)/client/red_cairo_canvas.h		\
	$(top_srcdir)/client/cmd_line_parser.cpp	\
	$(top_srcdir)/client/cmd_line_parser.h		\
	$(top_srcdir)/client/client_net_socket.cpp	\
	$(top_srcdir)/client/client_net_socket.h	\
	$(top_srcdir)/client/common.h			\
	$(top_srcdir)/client/cursor_channel.cpp		\
	$(top_srcdir)/client/cursor_channel.h		\
	$(top_srcdir)/client/cursor.cpp			\
	$(top_srcdir)/client/cursor.h			\
	$(top_srcdir)/client/debug.h			\
	$(top_srcdir)/client/display_channel.cpp	\
	$(top_srcdir)/client/display_channel.h		\
	$(top_srcdir)/client/red_gl_canvas.cpp		\
	$(top_srcdir)/client/red_gl_canvas.h		\
	$(top_srcdir)/client/gl_canvas.cpp		\
	$(top_srcdir)/client/glc.cpp			\
	$(top_srcdir)/client/glz_decoded_image.h	\
	$(top_srcdir)/client/glz_decoder_config.h	\
	$(top_srcdir)/client/glz_decoder.cpp		\
	$(top_srcdir)/client/glz_decoder.h		\
	$(top_srcdir)/client/glz_decoder_window.cpp	\
	$(top_srcdir)/client/glz_decoder_window.h	\
	$(top_srcdir)/client/inputs_channel.cpp		\
	$(top_srcdir)/client/inputs_channel.h		\
	$(top_srcdir)/client/inputs_handler.h		\
	$(top_srcdir)/client/lz.cpp			\
	$(top_srcdir)/client/lines.cpp			\
	$(top_srcdir)/client/monitor.cpp		\
	$(top_srcdir)/client/monitor.h			\
	$(top_srcdir)/client/menu.cpp			\
	$(top_srcdir)/client/menu.h			\
	$(top_srcdir)/client/pixels_source.h		\
	$(top_srcdir)/client/pixman_utils.cpp		\
	$(top_srcdir)/client/platform.h			\
	$(top_srcdir)/client/playback_channel.cpp	\
	$(top_srcdir)/client/process_loop.cpp		\
	$(top_srcdir)/client/quic.cpp			\
	$(top_srcdir)/client/record_channel.cpp		\
	$(top_srcdir)/client/red_channel.cpp		\
	$(top_srcdir)/client/red_channel.h		\
	$(top_srcdir)/client/red_client.cpp		\
	$(top_srcdir)/client/red_client.h		\
	$(top_srcdir)/client/red_drawable.h		\
	$(top_srcdir)/client/red_key.h			\
	$(top_srcdir)/client/red_peer.cpp		\
	$(top_srcdir)/client/red_peer.h			\
	$(top_srcdir)/client/red_pixmap_cairo.h		\
	$(top_srcdir)/client/red_pixmap_gl.h		\
	$(top_srcdir)/client/red_pixmap.h		\
	$(top_srcdir)/client/red_types.h		\
	$(top_srcdir)/client/red_window.h		\
	$(top_srcdir)/client/region.cpp			\
	$(top_srcdir)/client/rop3.cpp			\
	$(top_srcdir)/client/screen.cpp			\
	$(top_srcdir)/client/screen.h			\
	$(top_srcdir)/client/screen_layer.cpp		\
	$(top_srcdir)/client/screen_layer.h		\
	$(top_srcdir)/client/shared_cache.hpp		\
	$(top_srcdir)/client/hot_keys.cpp		\
	$(top_srcdir)/client/hot_keys.h			\
	$(top_srcdir)/client/threads.cpp		\
	$(top_srcdir)/client/tunnel_channel.cpp		\
	$(top_srcdir)/client/tunnel_channel.h		\
	$(top_srcdir)/client/utils.cpp			\
	$(top_srcdir)/client/utils.h			\
	$(top_srcdir)/client/icon.h                     \
	$(top_srcdir)/client/gui/softrenderer.h         \
	$(top_srcdir)/client/gui/softrenderer.cpp       \
	$(top_srcdir)/client/gui/softtexture.h		\
	$(top_srcdir)/client/gui/softtexture.cpp	\
	$(top_srcdir)/client/gui/resource_provider.h	\
	$(top_srcdir)/client/gui/resource_provider.cpp	\
	$(top_srcdir)/client/gui/gui.h			\
	$(top_srcdir)/client/gui/gui.cpp		\
	$(NULL)

bin_PROGRAMS = spicec

spicec_SOURCES =			\
	atomic_count.h			\
	event_sources_p.cpp		\
	main.cpp			\
	named_pipe.h                    \
	named_pipe.cpp                  \
	pixels_source.cpp		\
	pixels_source_p.h		\
	platform.cpp			\
	platform_utils.h                \
	platform_utils.cpp              \
	playback.cpp			\
	playback.h			\
	record.cpp			\
	record.h			\
	red_drawable.cpp		\
	red_pixmap.cpp			\
	red_pixmap_cairo.cpp		\
	red_pixmap_gl.cpp		\
	red_window.cpp			\
	red_window_p.h			\
	res.cpp				\
	res.h				\
	resource.h			\
	x_icon.cpp			\
	x_icon.h			\
	x_platform.h			\
	$(RED_COMMON_SRCS)		\
	$(NULL)

spicec_LDFLAGS = \
	$(SPICEC_STATIC_LINKAGE_BSTATIC)		\
	$(LOG4CPP_LIBS)					\
	$(CELT051_LIBS)					\
	$(SSL_LIBS)					\
	$(CEGUI_LIBS)

spicec_LDADD =						\
	$(QCAIRO_LIBS)					\
	$(FFMPEG_LIBS)					\
	$(ALSA_LIBS)					\
	$(GL_LIBS)					\
	$(XRANDR_LIBS)					\
	$(CEGUI_LIBS)					\
	-lrt

