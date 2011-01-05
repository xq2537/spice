NULL =

COMMON_DIR=$(SPICE_COMMON_SRCDIR)
CLIENT_DIR=$(top_srcdir)/client

SUBDIRS = images

INCLUDES = \
	-DSW_CANVAS_CACHE				\
	-DSW_CANVAS_NO_CHUNKS				\
	-DUSE_GLZ					\
	-D__STDC_LIMIT_MACROS				\
	-I.						\
	-I..						\
	-I$(COMMON_DIR)					\
	-I$(COMMON_DIR)/linux				\
	-I$(CLIENT_DIR)					\
	$(PROTOCOL_CFLAGS)				\
	$(GL_CFLAGS)					\
	$(ALSA_CFLAGS)					\
	$(PIXMAN_CFLAGS)				\
	$(CELT051_CFLAGS)				\
	$(SSL_CFLAGS)					\
	$(XRANDR_CFLAGS)				\
	$(XFIXES_CFLAGS)				\
	$(MISC_X_CFLAGS)				\
	$(CEGUI_CFLAGS)					\
	$(CEGUI06_CFLAGS)					\
	$(WARN_CFLAGS)                                  \
	$(SPICE_NONPKGCONFIG_CFLAGS)			\
	$(SMARTCARD_CFLAGS)				\
	$(NULL)


RED_COMMON_SRCS =					\
	$(CLIENT_DIR)/application.cpp			\
	$(CLIENT_DIR)/application.h			\
	$(CLIENT_DIR)/audio_channels.h			\
	$(CLIENT_DIR)/audio_devices.h			\
	$(CLIENT_DIR)/cache.hpp				\
	$(CLIENT_DIR)/demarshallers.h			\
	$(CLIENT_DIR)/generated_demarshallers.cpp	\
	$(CLIENT_DIR)/generated_demarshallers1.cpp	\
	$(CLIENT_DIR)/marshaller.cpp			\
	$(CLIENT_DIR)/marshallers.h			\
	$(CLIENT_DIR)/generated_marshallers.cpp		\
	$(CLIENT_DIR)/generated_marshallers1.cpp	\
	$(CLIENT_DIR)/sw_canvas.cpp			\
	$(CLIENT_DIR)/canvas.cpp			\
	$(CLIENT_DIR)/canvas.h				\
	$(CLIENT_DIR)/canvas_utils.cpp			\
	$(CLIENT_DIR)/red_sw_canvas.cpp			\
	$(CLIENT_DIR)/red_sw_canvas.h			\
	$(CLIENT_DIR)/cmd_line_parser.cpp		\
	$(CLIENT_DIR)/cmd_line_parser.h			\
	$(CLIENT_DIR)/client_net_socket.cpp		\
	$(CLIENT_DIR)/client_net_socket.h		\
	$(CLIENT_DIR)/common.h				\
	$(CLIENT_DIR)/controller.cpp			\
	$(CLIENT_DIR)/controller.h			\
	$(CLIENT_DIR)/cursor_channel.cpp		\
	$(CLIENT_DIR)/cursor_channel.h			\
	$(CLIENT_DIR)/cursor.cpp			\
	$(CLIENT_DIR)/cursor.h				\
	$(CLIENT_DIR)/debug.h				\
	$(CLIENT_DIR)/display_channel.cpp		\
	$(CLIENT_DIR)/display_channel.h			\
 	$(CLIENT_DIR)/foreign_menu.cpp			\
 	$(CLIENT_DIR)/foreign_menu.h			\
	$(CLIENT_DIR)/glz_decoded_image.h		\
	$(CLIENT_DIR)/glz_decoder_config.h		\
	$(CLIENT_DIR)/glz_decoder.cpp			\
	$(CLIENT_DIR)/glz_decoder.h			\
	$(CLIENT_DIR)/glz_decoder_window.cpp		\
	$(CLIENT_DIR)/glz_decoder_window.h		\
	$(CLIENT_DIR)/inputs_channel.cpp		\
	$(CLIENT_DIR)/inputs_channel.h			\
	$(CLIENT_DIR)/inputs_handler.h			\
	$(CLIENT_DIR)/jpeg_decoder.cpp			\
	$(CLIENT_DIR)/jpeg_decoder.h			\
	$(CLIENT_DIR)/lz.cpp				\
	$(CLIENT_DIR)/lines.cpp				\
	$(CLIENT_DIR)/monitor.cpp			\
	$(CLIENT_DIR)/monitor.h				\
	$(CLIENT_DIR)/mem.cpp				\
	$(CLIENT_DIR)/menu.cpp				\
	$(CLIENT_DIR)/menu.h				\
	$(CLIENT_DIR)/mjpeg_decoder.h			\
	$(CLIENT_DIR)/mjpeg_decoder.cpp			\
	$(CLIENT_DIR)/pixels_source.h			\
	$(CLIENT_DIR)/pixman_utils.cpp			\
	$(CLIENT_DIR)/platform.h			\
	$(CLIENT_DIR)/playback_channel.cpp		\
	$(CLIENT_DIR)/process_loop.cpp			\
	$(CLIENT_DIR)/quic.cpp				\
	$(CLIENT_DIR)/record_channel.cpp		\
	$(CLIENT_DIR)/red_channel.cpp			\
	$(CLIENT_DIR)/red_channel.h			\
	$(CLIENT_DIR)/red_client.cpp			\
	$(CLIENT_DIR)/red_client.h			\
	$(CLIENT_DIR)/red_drawable.h			\
	$(CLIENT_DIR)/red_key.h				\
	$(CLIENT_DIR)/red_peer.cpp			\
	$(CLIENT_DIR)/red_peer.h			\
	$(CLIENT_DIR)/red_pixmap_sw.h			\
	$(CLIENT_DIR)/red_pixmap.h			\
	$(CLIENT_DIR)/red_types.h			\
	$(CLIENT_DIR)/red_window.h			\
	$(CLIENT_DIR)/region.cpp			\
	$(CLIENT_DIR)/rop3.cpp				\
	$(CLIENT_DIR)/screen.cpp			\
	$(CLIENT_DIR)/screen.h				\
	$(CLIENT_DIR)/screen_layer.cpp			\
	$(CLIENT_DIR)/screen_layer.h			\
	$(CLIENT_DIR)/shared_cache.hpp			\
	$(CLIENT_DIR)/hot_keys.cpp			\
	$(CLIENT_DIR)/hot_keys.h			\
	$(CLIENT_DIR)/threads.cpp			\
	$(CLIENT_DIR)/utils.cpp				\
	$(CLIENT_DIR)/utils.h				\
	$(CLIENT_DIR)/zlib_decoder.cpp			\
	$(CLIENT_DIR)/zlib_decoder.h			\
	$(CLIENT_DIR)/icon.h				\
	$(NULL)

if SUPPORT_TUNNEL
RED_TUNNEL_SRCS =					\
	$(CLIENT_DIR)/tunnel_channel.cpp		\
	$(CLIENT_DIR)/tunnel_channel.h			\
	$(NULL)
else
RED_TUNNEL_SRCS =
endif

if SUPPORT_GUI
RED_GUI_SRCS =						\
	$(CLIENT_DIR)/gui/softrenderer.h		\
	$(CLIENT_DIR)/gui/softrenderer.cpp		\
	$(CLIENT_DIR)/gui/softtexture.h			\
	$(CLIENT_DIR)/gui/softtexture.cpp		\
	$(CLIENT_DIR)/gui/resource_provider.h		\
	$(CLIENT_DIR)/gui/resource_provider.cpp		\
	$(CLIENT_DIR)/gui/gui.h				\
	$(CLIENT_DIR)/gui/gui.cpp			\
	$(NULL)
else
RED_GUI_SRCS =
endif


if SUPPORT_GL
RED_OGL_SRCS =						\
	$(CLIENT_DIR)/gl_canvas.cpp			\
	$(CLIENT_DIR)/glc.cpp				\
	$(CLIENT_DIR)/red_gl_canvas.cpp			\
	$(CLIENT_DIR)/red_gl_canvas.h			\
	$(CLIENT_DIR)/red_pixmap_gl.h			\
	red_pixmap_gl.cpp
else
RED_OGL_SRCS =
endif

if SUPPORT_SMARTCARD
RED_SCARD_SRCS = $(CLIENT_DIR)/smartcard_channel.cpp
else
RED_SCARD_SRCS =
endif

bin_PROGRAMS = spicec

spicec_SOURCES =			\
	atomic_count.h			\
	event_sources_p.cpp		\
	event_sources_p.h		\
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
	red_pixmap_sw.cpp		\
	red_window.cpp			\
	red_window_p.h			\
	res.cpp				\
	res.h				\
	resource.h			\
	x_icon.cpp			\
	x_icon.h			\
	x_platform.h			\
	$(RED_COMMON_SRCS)		\
	$(RED_GUI_SRCS)			\
	$(RED_TUNNEL_SRCS)		\
	$(RED_OGL_SRCS)			\
	$(RED_SCARD_SRCS)		\
	$(NULL)

spicec_LDFLAGS = \
	$(SPICEC_STATIC_LINKAGE_BSTATIC)		\
	$(CELT051_LIBS)					\
	$(SSL_LIBS)					\
	$(CEGUI_LIBS)					\
	$(CEGUI06_LIBS)					\
	$(JPEG_LIBS)					\
	$(Z_LIBS)					\
	$(SMARTCARD_LIBS)				\
	$(SPICE_NONPKGCONFIG_LIBS)

spicec_LDADD =						\
	$(PIXMAN_LIBS)					\
	$(ALSA_LIBS)					\
	$(GL_LIBS)					\
	$(XRANDR_LIBS)					\
	$(XFIXES_LIBS)					\
	$(MISC_X_LIBS)					\
	$(CEGUI_LIBS)
