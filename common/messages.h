/*
   Copyright (C) 2009-2010 Red Hat, Inc.

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

#ifndef _H_MESSAGES
#define _H_MESSAGES

#include <spice/protocol.h>

#include <spice/start-packed.h>

typedef struct SPICE_ATTR_PACKED SpiceMsgData {
    uint32_t data_size;
    uint8_t data[0];
} SpiceMsgData;

typedef struct SPICE_ATTR_PACKED SpiceMsgEmpty {
} SpiceMsgEmpty;

typedef struct SPICE_ATTR_PACKED SpiceMsgInputsInit {
    uint32_t keyboard_modifiers;
} SpiceMsgInputsInit;

typedef struct SPICE_ATTR_PACKED SpiceMsgInputsKeyModifiers {
    uint32_t modifiers;
} SpiceMsgInputsKeyModifiers;

typedef struct SPICE_ATTR_PACKED SpiceMsgMainMultiMediaTime {
    uint32_t time;
} SpiceMsgMainMultiMediaTime;

typedef struct SPICE_ATTR_PACKED SpiceMsgMainMigrationBegin {
    uint16_t port;
    uint16_t sport;
    uint32_t host_offset;
    uint32_t host_size;
    uint16_t pub_key_type;
    uint32_t pub_key_offset;
    uint32_t pub_key_size;
} SpiceMsgMainMigrationBegin;

typedef struct SPICE_ATTR_PACKED SpiceMsgMainMigrationSwitchHost {
    uint16_t port;
    uint16_t sport;
    uint32_t host_offset;
    uint32_t host_size;
    uint32_t cert_subject_offset;
    uint32_t cert_subject_size;
} SpiceMsgMainMigrationSwitchHost;


typedef struct SPICE_ATTR_PACKED SpiceMsgMigrate {
    uint32_t flags;
} SpiceMsgMigrate;

typedef struct SPICE_ATTR_PACKED SpiceResourceID {
    uint8_t type;
    uint64_t id;
} SpiceResourceID;

typedef struct SPICE_ATTR_PACKED SpiceResourceList {
    uint16_t count;
    SpiceResourceID resources[0];
} SpiceResourceList;

typedef struct SPICE_ATTR_PACKED SpiceMsgSetAck {
    uint32_t generation;
    uint32_t window;
} SpiceMsgSetAck;

typedef struct SPICE_ATTR_PACKED SpiceMsgcAckSync {
  uint32_t generation;
} SpiceMsgcAckSync;

typedef struct SPICE_ATTR_PACKED SpiceWaitForChannel {
    uint8_t channel_type;
    uint8_t channel_id;
    uint64_t message_serial;
} SpiceWaitForChannel;

typedef struct SPICE_ATTR_PACKED SpiceMsgWaitForChannels {
    uint8_t wait_count;
    SpiceWaitForChannel wait_list[0];
} SpiceMsgWaitForChannels;

typedef struct SPICE_ATTR_PACKED SpiceChannelId {
    uint8_t type;
    uint8_t id;
} SpiceChannelId;

typedef struct SPICE_ATTR_PACKED SpiceMsgMainInit {
    uint32_t session_id;
    uint32_t display_channels_hint;
    uint32_t supported_mouse_modes;
    uint32_t current_mouse_mode;
    uint32_t agent_connected;
    uint32_t agent_tokens;
    uint32_t multi_media_time;
    uint32_t ram_hint;
} SpiceMsgMainInit;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisconnect {
    uint64_t time_stamp;
    uint32_t reason; // SPICE_ERR_?
} SpiceMsgDisconnect;

typedef struct SPICE_ATTR_PACKED SpiceMsgNotify {
    uint64_t time_stamp;
    uint32_t severity;
    uint32_t visibilty;
    uint32_t what;
    uint32_t message_len;
    uint8_t message[0];
} SpiceMsgNotify;

typedef struct SPICE_ATTR_PACKED SpiceMsgChannels {
    uint32_t num_of_channels;
    SpiceChannelId channels[0];
} SpiceMsgChannels;

typedef struct SPICE_ATTR_PACKED SpiceMsgMainMouseMode {
    uint32_t supported_modes;
    uint32_t current_mode;
} SpiceMsgMainMouseMode;

typedef struct SPICE_ATTR_PACKED SpiceMsgPing {
    uint32_t id;
    uint64_t timestamp;
} SpiceMsgPing;

typedef struct SPICE_ATTR_PACKED SpiceMsgMainAgentDisconnect {
    uint32_t error_code; // SPICE_ERR_?
} SpiceMsgMainAgentDisconnect;

#define SPICE_AGENT_MAX_DATA_SIZE 2048

typedef struct SPICE_ATTR_PACKED SpiceMsgMainAgentTokens {
    uint32_t num_tokens;
} SpiceMsgMainAgentTokens, SpiceMsgcMainAgentTokens, SpiceMsgcMainAgentStart;

typedef struct SPICE_ATTR_PACKED SpiceMsgcClientInfo {
    uint64_t cache_size;
} SpiceMsgcClientInfo;

typedef struct SPICE_ATTR_PACKED SpiceMsgcMainMouseModeRequest {
    uint32_t mode;
} SpiceMsgcMainMouseModeRequest;

typedef struct SPICE_ATTR_PACKED SpiceCursor {
    uint32_t flags;
    SpiceCursorHeader header;
    uint8_t data[0];
} SpiceCursor;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayMode {
    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
} SpiceMsgDisplayMode;

typedef struct SPICE_ATTR_PACKED SpiceMsgSurfaceCreate {
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;
} SpiceMsgSurfaceCreate;

typedef struct SPICE_ATTR_PACKED SpiceMsgSurfaceDestroy {
    uint32_t surface_id;
} SpiceMsgSurfaceDestroy;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayBase {
    uint32_t surface_id;
    SpiceRect box;
    SpiceClip clip;
} SpiceMsgDisplayBase;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawFill {
    SpiceMsgDisplayBase base;
    SpiceFill data;
} SpiceMsgDisplayDrawFill;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawOpaque {
    SpiceMsgDisplayBase base;
    SpiceOpaque data;
} SpiceMsgDisplayDrawOpaque;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawCopy {
    SpiceMsgDisplayBase base;
    SpiceCopy data;
} SpiceMsgDisplayDrawCopy;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawTransparent {
    SpiceMsgDisplayBase base;
    SpiceTransparent data;
} SpiceMsgDisplayDrawTransparent;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawAlphaBlend {
    SpiceMsgDisplayBase base;
    SpiceAlphaBlnd data;
} SpiceMsgDisplayDrawAlphaBlend;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayCopyBits {
    SpiceMsgDisplayBase base;
    SpicePoint src_pos;
} SpiceMsgDisplayCopyBits;

typedef SpiceMsgDisplayDrawCopy SpiceMsgDisplayDrawBlend;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawRop3 {
    SpiceMsgDisplayBase base;
    SpiceRop3 data;
} SpiceMsgDisplayDrawRop3;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawBlackness {
    SpiceMsgDisplayBase base;
    SpiceBlackness data;
} SpiceMsgDisplayDrawBlackness;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawWhiteness {
    SpiceMsgDisplayBase base;
    SpiceWhiteness data;
} SpiceMsgDisplayDrawWhiteness;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawInvers {
    SpiceMsgDisplayBase base;
    SpiceInvers data;
} SpiceMsgDisplayDrawInvers;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawStroke {
    SpiceMsgDisplayBase base;
    SpiceStroke data;
} SpiceMsgDisplayDrawStroke;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayDrawText {
    SpiceMsgDisplayBase base;
    SpiceText data;
} SpiceMsgDisplayDrawText;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayInvalOne {
    uint64_t id;
} SpiceMsgDisplayInvalOne;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayStreamCreate {
    uint32_t surface_id;
    uint32_t id;
    uint32_t flags;
    uint32_t codec_type;
    uint64_t stamp;
    uint32_t stream_width;
    uint32_t stream_height;
    uint32_t src_width;
    uint32_t src_height;
    SpiceRect dest;
    SpiceClip clip;
} SpiceMsgDisplayStreamCreate;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayStreamData {
    uint32_t id;
    uint32_t multi_media_time;
    uint32_t data_size;
    uint32_t pad_size;
    uint8_t data[0];
} SpiceMsgDisplayStreamData;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayStreamClip {
    uint32_t id;
    SpiceClip clip;
} SpiceMsgDisplayStreamClip;

typedef struct SPICE_ATTR_PACKED SpiceMsgDisplayStreamDestroy {
    uint32_t id;
} SpiceMsgDisplayStreamDestroy;

typedef struct SPICE_ATTR_PACKED SpiceMsgCursorInit {
    SpicePoint16 position;
    uint16_t trail_length;
    uint16_t trail_frequency;
    uint8_t visible;
    SpiceCursor cursor;
} SpiceMsgCursorInit;

typedef struct SPICE_ATTR_PACKED SpiceMsgCursorSet {
    SpicePoint16 position;
    uint8_t visible;
    SpiceCursor cursor;
} SpiceMsgCursorSet;

typedef struct SPICE_ATTR_PACKED SpiceMsgCursorMove {
    SpicePoint16 position;
} SpiceMsgCursorMove;

typedef struct SPICE_ATTR_PACKED SpiceMsgCursorTrail {
    uint16_t length;
    uint16_t frequency;
} SpiceMsgCursorTrail;

typedef struct SPICE_ATTR_PACKED SpiceMsgcDisplayInit {
    uint8_t pixmap_cache_id;
    int64_t pixmap_cache_size; //in pixels
    uint8_t glz_dictionary_id;
    int32_t glz_dictionary_window_size;       // in pixels
} SpiceMsgcDisplayInit;

typedef struct SPICE_ATTR_PACKED SpiceMsgcKeyDown {
    uint32_t code;
} SpiceMsgcKeyDown;

typedef struct SPICE_ATTR_PACKED SpiceMsgcKeyUp {
    uint32_t code;
} SpiceMsgcKeyUp;

typedef struct SPICE_ATTR_PACKED SpiceMsgcKeyModifiers {
    uint32_t modifiers;
} SpiceMsgcKeyModifiers;

typedef struct SPICE_ATTR_PACKED SpiceMsgcMouseMotion {
    int32_t dx;
    int32_t dy;
    uint32_t buttons_state;
} SpiceMsgcMouseMotion;

typedef struct SPICE_ATTR_PACKED SpiceMsgcMousePosition {
    uint32_t x;
    uint32_t y;
    uint32_t buttons_state;
    uint8_t display_id;
} SpiceMsgcMousePosition;

typedef struct SPICE_ATTR_PACKED SpiceMsgcMousePress {
    int32_t button;
    int32_t buttons_state;
} SpiceMsgcMousePress;

typedef struct SPICE_ATTR_PACKED SpiceMsgcMouseRelease {
    int32_t button;
    int32_t buttons_state;
} SpiceMsgcMouseRelease;

typedef struct SPICE_ATTR_PACKED SpiceMsgPlaybackMode {
    uint32_t time;
    uint32_t mode; //SPICE_AUDIO_DATA_MODE_?
    uint8_t data[0];
} SpiceMsgPlaybackMode, SpiceMsgcRecordMode;

typedef struct SPICE_ATTR_PACKED SpiceMsgPlaybackStart {
    uint32_t channels;
    uint32_t format; //SPICE_AUDIO_FMT_?
    uint32_t frequency;
    uint32_t time;
} SpiceMsgPlaybackStart;

typedef struct SPICE_ATTR_PACKED SpiceMsgPlaybackPacket {
    uint32_t time;
    uint8_t data[0];
} SpiceMsgPlaybackPacket, SpiceMsgcRecordPacket;

typedef struct SPICE_ATTR_PACKED SpiceMsgRecordStart {
    uint32_t channels;
    uint32_t format; //SPICE_AUDIO_FMT_?
    uint32_t frequency;
} SpiceMsgRecordStart;

typedef struct SPICE_ATTR_PACKED SpiceMsgcRecordStartMark {
    uint32_t time;
} SpiceMsgcRecordStartMark;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelInit {
    uint16_t max_num_of_sockets;
    uint32_t max_socket_data_size;
} SpiceMsgTunnelInit;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelIpInfo {
    uint16_t type;
    uint8_t data[0];
} SpiceMsgTunnelIpInfo;

typedef uint8_t SpiceTunnelIPv4[4];

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelServiceIpMap {
    uint32_t service_id;
    SpiceMsgTunnelIpInfo virtual_ip;
} SpiceMsgTunnelServiceIpMap;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelSocketOpen {
    uint16_t connection_id;
    uint32_t service_id;
    uint32_t tokens;
} SpiceMsgTunnelSocketOpen;

/* connection id must be the first field in msgs directed to a specific connection */

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelSocketFin {
    uint16_t connection_id;
} SpiceMsgTunnelSocketFin;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelSocketClose {
    uint16_t connection_id;
} SpiceMsgTunnelSocketClose;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelSocketData {
    uint16_t connection_id;
    uint8_t data[0];
} SpiceMsgTunnelSocketData;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelSocketTokens {
    uint16_t connection_id;
    uint32_t num_tokens;
} SpiceMsgTunnelSocketTokens;

typedef struct SPICE_ATTR_PACKED SpiceMsgTunnelSocketClosedAck {
    uint16_t connection_id;
} SpiceMsgTunnelSocketClosedAck;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelAddGenericService {
    uint32_t type;
    uint32_t id;
    uint32_t group;
    uint32_t port;
    uint32_t name;
    uint32_t description;
} SpiceMsgcTunnelAddGenericService;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelAddPrintService {
    SpiceMsgcTunnelAddGenericService base;
    SpiceMsgTunnelIpInfo ip;
} SpiceMsgcTunnelAddPrintService;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelRemoveService {
    uint32_t id;
} SpiceMsgcTunnelRemoveService;

/* connection id must be the first field in msgs directed to a specific connection */

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketOpenAck {
    uint16_t connection_id;
    uint32_t tokens;
} SpiceMsgcTunnelSocketOpenAck;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketOpenNack {
    uint16_t connection_id;
} SpiceMsgcTunnelSocketOpenNack;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketData {
    uint16_t connection_id;
    uint8_t data[0];
} SpiceMsgcTunnelSocketData;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketFin {
    uint16_t connection_id;
} SpiceMsgcTunnelSocketFin;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketClosed {
    uint16_t connection_id;
} SpiceMsgcTunnelSocketClosed;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketClosedAck {
    uint16_t connection_id;
} SpiceMsgcTunnelSocketClosedAck;

typedef struct SPICE_ATTR_PACKED SpiceMsgcTunnelSocketTokens {
    uint16_t connection_id;
    uint32_t num_tokens;
} SpiceMsgcTunnelSocketTokens;

#include <spice/end-packed.h>

#endif /* _H_SPICE_PROTOCOL */


