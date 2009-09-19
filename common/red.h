/*
   Copyright (C) 2009 Red Hat, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in
         the documentation and/or other materials provided with the
         distribution.
       * Neither the name of the copyright holder nor the names of its
         contributors may be used to endorse or promote products derived
         from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _H_RED
#define _H_RED

#include <stdint.h>
#ifdef _WIN32
#include <basetsd.h>
#endif
#include "draw.h"
#ifdef __GNUC__
#define ATTR_PACKED __attribute__ ((__packed__))
#else
#pragma pack(push)
#pragma pack(1)
#define ATTR_PACKED
#endif

#define RED_MAGIC (*(uint32_t*)"REDQ")
#define RED_VERSION_MAJOR 1
#define RED_VERSION_MINOR 0

// Encryption & Ticketing Parameters
#define RED_MAX_PASSWORD_LENGTH 60
#define RED_TICKET_KEY_PAIR_LENGTH 1024
#define RED_TICKET_PUBKEY_BYTES (RED_TICKET_KEY_PAIR_LENGTH / 8 + 34)

enum {
    RED_CHANNEL_MAIN = 1,
    RED_CHANNEL_DISPLAY,
    RED_CHANNEL_INPUTS,
    RED_CHANNEL_CURSOR,
    RED_CHANNEL_PLAYBACK,
    RED_CHANNEL_RECORD,

    RED_CHANNEL_END
};

enum {
    RED_ERR_OK,
    RED_ERR_ERROR,
    RED_ERR_INVALID_MAGIC,
    RED_ERR_INVALID_DATA,
    RED_ERR_VERSION_MISMATCH,
    RED_ERR_NEED_SECURED,
    RED_ERR_NEED_UNSECURED,
    RED_ERR_PERMISSION_DENIED,
    RED_ERR_BAD_CONNECTION_ID,
    RED_ERR_CHANNEL_NOT_AVAILABLE,
};

enum {
    RED_WARN_GENERAL,
};

enum {
    RED_INFO_GENERAL,
};

typedef struct ATTR_PACKED RedLinkHeader {
    uint32_t magic;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t size;
} RedLinkHeader;

typedef struct ATTR_PACKED RedLinkMess {
    uint32_t connection_id;
    uint8_t channel_type;
    uint8_t channel_id;
    uint32_t num_common_caps;
    uint32_t num_channel_caps;
    uint32_t caps_offset;
} RedLinkMess;

typedef struct ATTR_PACKED RedLinkReply {
    uint32_t error;
    uint8_t pub_key[RED_TICKET_PUBKEY_BYTES];
    uint32_t num_common_caps;
    uint32_t num_channel_caps;
    uint32_t caps_offset;
} RedLinkReply;

typedef struct ATTR_PACKED RedLinkEncryptedTicket {
    uint8_t encrypted_data[RED_TICKET_KEY_PAIR_LENGTH / 8];
} RedLinkEncryptedTicket;

typedef struct ATTR_PACKED RedDataHeader {
    uint64_t serial;
    uint16_t type;
    uint32_t size;
    uint32_t sub_list; //offset to RedSubMessageList[]
} RedDataHeader;

typedef struct ATTR_PACKED RedSubMessage {
    uint16_t type;
    uint32_t size;
} RedSubMessage;

typedef struct ATTR_PACKED RedSubMessageList {
    uint16_t size;
    uint32_t sub_messages[0]; //offsets to RedSubMessage
} RedSubMessageList;

enum {
    RED_MIGRATE = 1,
    RED_MIGRATE_DATA,
    RED_SET_ACK,
    RED_PING,
    RED_WAIT_FOR_CHANNELS,
    RED_DISCONNECTING,
    RED_NOTIFY,

    RED_FIRST_AVAIL_MESSAGE = 101
};

enum {
    REDC_ACK_SYNC = 1,
    REDC_ACK,
    REDC_PONG,
    REDC_MIGRATE_FLUSH_MARK,
    REDC_MIGRATE_DATA,
    REDC_DISCONNECTING,

    REDC_FIRST_AVAIL_MESSAGE = 101,
};

enum {
    RED_MIGRATE_BEGIN = RED_FIRST_AVAIL_MESSAGE,
    RED_MIGRATE_CANCEL,
    RED_INIT,
    RED_CHANNELS_LIST,
    RED_MOUSE_MODE,
    RED_MULTI_MEDIA_TIME,

    RED_AGENT_CONNECTED,
    RED_AGENT_DISCONNECTED,
    RED_AGENT_DATA,
    RED_AGENT_TOKEN,

    RED_MESSAGES_END,
};

enum {
    REDC_CLIENT_INFO = REDC_FIRST_AVAIL_MESSAGE,
    REDC_MIGRATE_CONNECTED,
    REDC_MIGRATE_CONNECT_ERROR,
    REDC_ATTACH_CHANNELS,
    REDC_MOUSE_MODE_REQUEST,

    REDC_AGENT_START,
    REDC_AGENT_DATA,
    REDC_AGENT_TOKEN,
};

#define RED_MOTION_ACK_BUNCH 4

enum {
    RED_INPUTS_INIT = RED_FIRST_AVAIL_MESSAGE,
    RED_INPUTS_KEY_MODIFAIERS,

    RED_INPUTS_MOUSE_MOTION_ACK = RED_FIRST_AVAIL_MESSAGE + 10,

    RED_INPUTS_MESSAGES_END,
};

#define RED_SCROLL_LOCK_MODIFIER (1 << 0)
#define RED_NUM_LOCK_MODIFIER (1 << 1)
#define RED_CAPS_LOCK_MODIFIER (1 << 2)

typedef struct ATTR_PACKED RedInputsInit {
    uint32_t keyboard_modifiers;
} RedInputsInit;

typedef struct ATTR_PACKED RedKeyModifiers {
    uint32_t modifiers;
} RedKeyModifiers;

typedef struct ATTR_PACKED RedMultiMediaTime {
    uint32_t time;
} RedMultiMediaTime;

typedef struct ATTR_PACKED RedMigrationBegin {
    uint16_t port;
    uint16_t sport;
    char host[0];
} RedMigrationBegin;

enum {
    RED_MIGRATE_NEED_FLUSH = (1 << 0),
    RED_MIGRATE_NEED_DATA_TRANSFER = (1 << 1),
};

typedef struct ATTR_PACKED RedMigrate {
    uint32_t flags;
} RedMigrate;

enum {
    RED_RES_TYPE_INVALID,
    RED_RES_TYPE_PIXMAP,
};

typedef struct ATTR_PACKED RedResorceID {
    uint8_t type;
    uint64_t id;
} RedResorceID;

typedef struct ATTR_PACKED RedResorceList {
    uint16_t count;
    RedResorceID resorces[0];
} RedResorceList;

typedef struct ATTR_PACKED RedSetAck {
    uint32_t generation;
    uint32_t window;
} RedSetAck;

typedef struct ATTR_PACKED RedWaitForChannel {
    uint8_t channel_type;
    uint8_t channel_id;
    uint64_t message_serial;
} RedWaitForChannel;

typedef struct ATTR_PACKED RedWaitForChannels {
    uint8_t wait_count;
    RedWaitForChannel wait_list[0];
} RedWaitForChannels;

typedef struct ATTR_PACKED RedChannelInit {
    uint8_t type;
    uint8_t id;
} RedChannelInit;

typedef struct ATTR_PACKED RedInit {
    uint32_t session_id;
    uint32_t display_channels_hint;
    uint32_t supported_mouse_modes;
    uint32_t current_mouse_mode;
    uint32_t agent_connected;
    uint32_t agent_tokens;
    uint32_t multi_media_time;
    uint32_t ram_hint;
} RedInit;

typedef struct ATTR_PACKED RedDisconnect {
    uint64_t time_stamp;
    uint32_t reason; // RED_ERR_?
} RedDisconnect;

enum {
    RED_NOTIFY_SEVERITY_INFO,
    RED_NOTIFY_SEVERITY_WARN,
    RED_NOTIFY_SEVERITY_ERROR,
};

enum {
    RED_NOTIFY_VISIBILITY_LOW,
    RED_NOTIFY_VISIBILITY_MEDIUM,
    RED_NOTIFY_VISIBILITY_HIGH,
};

typedef struct ATTR_PACKED RedNotify {
    uint64_t time_stamp;
    uint32_t severty;
    uint32_t visibilty;
    uint32_t what;
    uint32_t message_len;
    uint8_t message[0];
} RedNotify;

typedef struct ATTR_PACKED RedChannels {
    uint32_t num_of_channels;
    RedChannelInit channels[0];
} RedChannels;

typedef struct ATTR_PACKED RedMouseMode {
    uint32_t supported_modes;
    uint32_t current_mode;
} RedMouseMode;

typedef struct ATTR_PACKED RedPing {
    uint32_t id;
    uint64_t timestamp;
} RedPing;

typedef struct ATTR_PACKED RedAgentDisconnect {
    uint32_t error_code; // RED_ERR_?
} RedAgentDisconnect;

#define RED_AGENT_MAX_DATA_SIZE 2048

typedef struct ATTR_PACKED RedAgentTokens {
    uint32_t num_tokens;
} RedAgentTokens, RedcAgentTokens, RedcAgentStart;

typedef struct ATTR_PACKED RedcClientInfo {
    uint64_t cache_size;
} RedcClientInfo;

typedef struct ATTR_PACKED RedcMouseModeRequest {
    uint32_t mode;
} RedcMouseModeRequest;

enum {
    RED_DISPLAY_MODE = RED_FIRST_AVAIL_MESSAGE,
    RED_DISPLAY_MARK,
    RED_DISPLAY_RESET,
    RED_DISPLAY_COPY_BITS,

    RED_DISPLAY_INVAL_LIST,
    RED_DISPLAY_INVAL_ALL_PIXMAPS,
    RED_DISPLAY_INVAL_PALETTE,
    RED_DISPLAY_INVAL_ALL_PALETTES,

    RED_DISPLAY_STREAM_CREATE = RED_FIRST_AVAIL_MESSAGE + 21,
    RED_DISPLAY_STREAM_DATA,
    RED_DISPLAY_STREAM_CLIP,
    RED_DISPLAY_STREAM_DESTROY,
    RED_DISPLAY_STREAM_DESTROY_ALL,

    RED_DISPLAY_DRAW_FILL = RED_FIRST_AVAIL_MESSAGE + 201,
    RED_DISPLAY_DRAW_OPAQUE,
    RED_DISPLAY_DRAW_COPY,
    RED_DISPLAY_DRAW_BLEND,
    RED_DISPLAY_DRAW_BLACKNESS,
    RED_DISPLAY_DRAW_WHITENESS,
    RED_DISPLAY_DRAW_INVERS,
    RED_DISPLAY_DRAW_ROP3,
    RED_DISPLAY_DRAW_STROKE,
    RED_DISPLAY_DRAW_TEXT,
    RED_DISPLAY_DRAW_TRANSPARENT,
    RED_DISPLAY_DRAW_ALPHA_BLEND,

    RED_DISPLAY_MESSAGES_END,
};

enum {
    RED_CURSOR_NONE = (1 << 0),
    RED_CURSOR_CACHE_ME = (1 << 1),
    RED_CURSOR_FROM_CACHE = (1 << 2),
};

typedef struct ATTR_PACKED RedCursor {
    uint32_t flags;
    CursorHeader header;
    uint8_t data[0];
} RedCursor;

typedef struct ATTR_PACKED RedMode {
    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
} RedMode;

typedef struct ATTR_PACKED RedDrawBase {
    Rect box;
    Clip clip;
} RedDrawBase;

typedef struct ATTR_PACKED RedFill {
    RedDrawBase base;
    Fill data;
} RedFill;

typedef struct ATTR_PACKED RedOpaque {
    RedDrawBase base;
    Opaque data;
} RedOpaque;

typedef struct ATTR_PACKED RedCopy {
    RedDrawBase base;
    Copy data;
} RedCopy;

typedef struct ATTR_PACKED RedTransparent {
    RedDrawBase base;
    Transparent data;
} RedTransparent;

typedef struct ATTR_PACKED RedAlphaBlend {
    RedDrawBase base;
    AlphaBlnd data;
} RedAlphaBlend;

typedef struct ATTR_PACKED RedCopyBits {
    RedDrawBase base;
    Point src_pos;
} RedCopyBits;

typedef RedCopy RedBlend;

typedef struct ATTR_PACKED RedRop3 {
    RedDrawBase base;
    Rop3 data;
} RedRop3;

typedef struct ATTR_PACKED RedBlackness {
    RedDrawBase base;
    Blackness data;
} RedBlackness;

typedef struct ATTR_PACKED RedWhiteness {
    RedDrawBase base;
    Whiteness data;
} RedWhiteness;

typedef struct ATTR_PACKED RedInvers {
    RedDrawBase base;
    Invers data;
} RedInvers;

typedef struct ATTR_PACKED RedStroke {
    RedDrawBase base;
    Stroke data;
} RedStroke;

typedef struct ATTR_PACKED RedText {
    RedDrawBase base;
    Text data;
} RedText;

typedef struct ATTR_PACKED RedInvalOne {
    uint64_t id;
} RedInvalOne;

enum {
    RED_VIDEO_CODEC_TYPE_MJPEG = 1,
};

enum {
    STREAM_TOP_DOWN = (1 << 0),
};

typedef struct ATTR_PACKED RedStreamCreate {
    uint32_t id;
    uint32_t flags;
    uint32_t codec_type;
    uint64_t stamp;
    uint32_t stream_width;
    uint32_t stream_height;
    uint32_t src_width;
    uint32_t src_height;
    Rect dest;
    Clip clip;
} RedStreamCreate;

typedef struct ATTR_PACKED RedStreamData {
    uint32_t id;
    uint32_t multi_media_time;
    uint32_t data_size;
    uint32_t ped_size;
    uint8_t data[0];
} RedStreamData;

typedef struct ATTR_PACKED RedStreamClip {
    uint32_t id;
    Clip clip;
} RedStreamClip;

typedef struct ATTR_PACKED RedStreamDestroy {
    uint32_t id;
} RedStreamDestroy;

enum {
    RED_CURSOR_INIT = RED_FIRST_AVAIL_MESSAGE,
    RED_CURSOR_RESET,
    RED_CURSOR_SET,
    RED_CURSOR_MOVE,
    RED_CURSOR_HIDE,
    RED_CURSOR_TRAIL,
    RED_CURSOR_INVAL_ONE,
    RED_CURSOR_INVAL_ALL,

    RED_CURSOR_MESSAGES_END,
};

typedef struct ATTR_PACKED RedCursorInit {
    Point16 position;
    uint16_t trail_length;
    uint16_t trail_frequency;
    uint8_t visible;
    RedCursor cursor;
} RedCursorInit;

typedef struct ATTR_PACKED RedCursorSet {
    Point16 postition;
    uint8_t visible;
    RedCursor cursor;
} RedCursorSet;

typedef struct ATTR_PACKED RedCursorMove {
    Point16 postition;
} RedCursorMove;

typedef struct ATTR_PACKED RedCursorTrail {
    uint16_t length;
    uint16_t frequency;
} RedCursorTrail;

enum {
    REDC_DISPLAY_INIT = REDC_FIRST_AVAIL_MESSAGE,

    REDC_DISPLAY_MESSGES_END,
};

typedef struct ATTR_PACKED RedcDisplayInit {
    uint8_t pixmap_cache_id;
    int64_t pixmap_cache_size; //in pixels
    uint8_t glz_dictionary_id;
    int glz_dictionary_window_size;       // in pixels
} RedcDisplayInit;

enum {
    REDC_INPUTS_KEY_DOWN = REDC_FIRST_AVAIL_MESSAGE,
    REDC_INPUTS_KEY_UP,
    REDC_INPUTS_KEY_MODIFAIERS,

    REDC_INPUTS_MOUSE_MOTION = REDC_FIRST_AVAIL_MESSAGE + 10,
    REDC_INPUTS_MOUSE_POSITION,
    REDC_INPUTS_MOUSE_PRESS,
    REDC_INPUTS_MOUSE_RELEASE,

    REDC_INPUTS_MESSGES_END,
};

typedef struct ATTR_PACKED RedcKeyDown {
    uint32_t code;
} RedcKeyDown;

typedef struct ATTR_PACKED RedcKeyUp {
    uint32_t code;
} RedcKeyUp;

enum {
    RED_MOUSE_MODE_SERVER = (1 << 0),
    RED_MOUSE_MODE_CLIENT = (1 << 1),
};

typedef struct ATTR_PACKED RedcKeyModifiers {
    uint32_t modifiers;
} RedcKeyModifiers;

enum RedButton {
    REDC_MOUSE_INVALID_BUTTON,
    REDC_MOUSE_LBUTTON,
    REDC_MOUSE_MBUTTON,
    REDC_MOUSE_RBUTTON,
    REDC_MOUSE_UBUTTON,
    REDC_MOUSE_DBUTTON,
};

#define REDC_LBUTTON_MASK (1 << (REDC_MOUSE_LBUTTON - 1))
#define REDC_MBUTTON_MASK (1 << (REDC_MOUSE_MBUTTON - 1))
#define REDC_RBUTTON_MASK (1 << (REDC_MOUSE_RBUTTON - 1))

typedef struct ATTR_PACKED RedcMouseMotion {
    int32_t dx;
    int32_t dy;
    uint32_t buttons_state;
} RedcMouseMotion;

typedef struct ATTR_PACKED RedcMousePosition {
    uint32_t x;
    uint32_t y;
    uint32_t buttons_state;
    uint8_t display_id;
} RedcMousePosition;

typedef struct ATTR_PACKED RedcMousePress {
    int32_t button;
    int32_t buttons_state;
} RedcMousePress;

typedef struct ATTR_PACKED RedcMouseRelease {
    int32_t button;
    int32_t buttons_state;
} RedcMouseRelease;

enum {
    RED_AUDIO_FMT_INVALD,
    RED_AUDIO_FMT_S16,
};

enum {
    RED_AUDIO_DATA_MODE_INVALD,
    RED_AUDIO_DATA_MODE_RAW,
    RED_AUDIO_DATA_MODE_CELT_0_5_1,
};

enum {
    RED_PLAYBACK_DATA = RED_FIRST_AVAIL_MESSAGE,
    RED_PLAYBACK_MODE,
    RED_PLAYBACK_START,
    RED_PLAYBACK_STOP,

    RED_PLAYBACK_MESSAGES_END,
};

enum {
    RED_PLAYBACK_CAP_CELT_0_5_1,
};

enum {
    RED_RECORD_START = RED_FIRST_AVAIL_MESSAGE,
    RED_RECORD_STOP,

    RED_RECORD_MESSAGES_END,
};

enum {
    REDC_RECORD_DATA = RED_FIRST_AVAIL_MESSAGE,
    REDC_RECORD_MODE,
    REDC_RECORD_START_MARK,

    REDC_RECORD_MESSAGES_END,
};

enum {
    RED_RECORD_CAP_CELT_0_5_1,
};

typedef struct ATTR_PACKED RedPlaybackMode {
    uint32_t time;
    uint32_t mode; //RED_AUDIO_DATA_MODE_?
    uint8_t data[0];
} RedPlaybackMode, RedcRecordMode;

typedef struct ATTR_PACKED RedPlaybackStart {
    uint32_t channels;
    uint32_t format; //RED_AUDIO_FMT_?
    uint32_t frequency;
    uint32_t time;
} RedPlaybackStart;

typedef struct ATTR_PACKED RedPlaybackPacket {
    uint32_t time;
    uint8_t data[0];
} RedPlaybackPacket, RedcRecordPacket;

typedef struct ATTR_PACKED RedRecordStart {
    uint32_t channels;
    uint32_t format; //RED_AUDIO_FMT_?
    uint32_t frequency;
} RedRecordStart;

typedef struct ATTR_PACKED RedcRecordStartMark {
    uint32_t time;
} RedcRecordStartMark;

#undef ATTR_PACKED

#ifndef __GNUC__
#pragma pack(pop)
#endif

#endif

