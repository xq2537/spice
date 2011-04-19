/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <celt051/celt.h>

#include "spice.h"
#include "red_common.h"
#include "reds.h"
#include "red_dispatcher.h"
#include "snd_worker.h"
#include "marshaller.h"
#include "generated_marshallers.h"
#include "demarshallers.h"

#define MAX_SEND_VEC 100

#define RECIVE_BUF_SIZE (16 * 1024 * 2)

#define FRAME_SIZE 256
#define PLAYBACK_BUF_SIZE (FRAME_SIZE * 4)

#define CELT_BIT_RATE (64 * 1024)
#define CELT_COMPRESSED_FRAME_BYTES (FRAME_SIZE * CELT_BIT_RATE / SPICE_INTERFACE_PLAYBACK_FREQ / 8)

#define RECORD_SAMPLES_SIZE (RECIVE_BUF_SIZE >> 2)

enum PlaybackeCommand {
    SND_PLAYBACK_MIGRATE,
    SND_PLAYBACK_MODE,
    SND_PLAYBACK_CTRL,
    SND_PLAYBACK_PCM,
};

enum RecordCommand {
    SND_RECORD_MIGRATE,
    SND_RECORD_CTRL,
};

#define SND_PLAYBACK_MIGRATE_MASK (1 << SND_PLAYBACK_MIGRATE)
#define SND_PLAYBACK_MODE_MASK (1 << SND_PLAYBACK_MODE)
#define SND_PLAYBACK_CTRL_MASK (1 << SND_PLAYBACK_CTRL)
#define SND_PLAYBACK_PCM_MASK (1 << SND_PLAYBACK_PCM)

#define SND_RECORD_MIGRATE_MASK (1 << SND_RECORD_MIGRATE)
#define SND_RECORD_CTRL_MASK (1 << SND_RECORD_CTRL)

typedef struct SndChannel SndChannel;
typedef void (*send_messages_proc)(void *in_channel);
typedef int (*handle_message_proc)(SndChannel *channel, size_t size, uint32_t type, void *message);
typedef void (*on_message_done_proc)(SndChannel *channel);
typedef void (*cleanup_channel_proc)(SndChannel *channel);

typedef struct SndWorker SndWorker;

struct SndChannel {
    RedsStream *stream;
    SndWorker *worker;
    spice_parse_channel_func_t parser;

    int active;
    int client_active;
    int blocked;

    uint32_t command;
    int migrate;
    uint32_t ack_generation;
    uint32_t client_ack_generation;
    uint32_t out_messages;
    uint32_t ack_messages;

    struct {
        uint64_t serial;
        SpiceDataHeader *header;
        SpiceMarshaller *marshaller;
        uint32_t size;
        uint32_t pos;
    } send_data;

    struct {
        uint8_t buf[RECIVE_BUF_SIZE];
        SpiceDataHeader *message;
        uint8_t *now;
        uint8_t *end;
    } recive_data;

    send_messages_proc send_messages;
    handle_message_proc handle_message;
    on_message_done_proc on_message_done;
    cleanup_channel_proc cleanup;
};

typedef struct AudioFrame AudioFrame;
struct AudioFrame {
    uint32_t time;
    uint32_t samples[FRAME_SIZE];
    AudioFrame *next;
};

typedef struct PlaybackChannel {
    SndChannel base;
    AudioFrame frames[3];
    AudioFrame *free_frames;
    AudioFrame *in_progress;
    AudioFrame *pending_frame;
    CELTMode *celt_mode;
    CELTEncoder *celt_encoder;
    int celt_allowed;
    uint32_t mode;
    struct {
        uint8_t celt_buf[CELT_COMPRESSED_FRAME_BYTES];
    } send_data;
} PlaybackChannel;

struct SndWorker {
    Channel base;
    SndChannel *connection;
    SndWorker *next;
    int active;
};

struct SpicePlaybackState {
    struct SndWorker worker;
    SpicePlaybackInstance *sin;
};

struct SpiceRecordState {
    struct SndWorker worker;
    SpiceRecordInstance *sin;
};

#define RECORD_MIG_VERSION 1

typedef struct __attribute__ ((__packed__)) RecordMigrateData {
    uint32_t version;
    uint64_t serial;
    uint32_t start_time;
    uint32_t mode;
    uint32_t mode_time;
} RecordMigrateData;

typedef struct RecordChannel {
    SndChannel base;
    uint32_t samples[RECORD_SAMPLES_SIZE];
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t mode;
    uint32_t mode_time;
    uint32_t start_time;
    CELTDecoder *celt_decoder;
    CELTMode *celt_mode;
    uint32_t celt_buf[FRAME_SIZE];
} RecordChannel;

static SndWorker *workers = NULL;
static uint32_t playback_compression = SPICE_AUDIO_DATA_MODE_CELT_0_5_1;

static void snd_receive(void* data);

static void snd_disconnect_channel(SndChannel *channel)
{
    SndWorker *worker;

    if (!channel) {
        return;
    }
    channel->cleanup(channel);
    worker = channel->worker;
    worker->connection = NULL;
    core->watch_remove(channel->stream->watch);
    channel->stream->watch = NULL;
    reds_stream_free(channel->stream);
    spice_marshaller_destroy(channel->send_data.marshaller);
    free(channel);
}

static void snd_playback_free_frame(PlaybackChannel *playback_channel, AudioFrame *frame)
{
    frame->next = playback_channel->free_frames;
    playback_channel->free_frames = frame;
}

static void snd_playback_on_message_done(SndChannel *channel)
{
    PlaybackChannel *playback_channel = (PlaybackChannel *)channel;
    if (playback_channel->in_progress) {
        snd_playback_free_frame(playback_channel, playback_channel->in_progress);
        playback_channel->in_progress = NULL;
        if (playback_channel->pending_frame) {
            channel->command |= SND_PLAYBACK_PCM_MASK;
        }
    }
}

static void snd_record_on_message_done(SndChannel *channel)
{
}

static int snd_send_data(SndChannel *channel)
{
    uint32_t n;

    if (!channel) {
        return FALSE;
    }

    if (!(n = channel->send_data.size - channel->send_data.pos)) {
        return TRUE;
    }

    for (;;) {
        struct iovec vec[MAX_SEND_VEC];
        int vec_size;

        if (!n) {
            channel->on_message_done(channel);

            if (channel->blocked) {
                channel->blocked = FALSE;
                core->watch_update_mask(channel->stream->watch, SPICE_WATCH_EVENT_READ);
            }
            break;
        }

        vec_size = spice_marshaller_fill_iovec(channel->send_data.marshaller,
                                               vec, MAX_SEND_VEC, channel->send_data.pos);
        n = reds_stream_writev(channel->stream, vec, vec_size);
        if (n == -1) {
            switch (errno) {
            case EAGAIN:
                channel->blocked = TRUE;
                core->watch_update_mask(channel->stream->watch, SPICE_WATCH_EVENT_READ |
                                        SPICE_WATCH_EVENT_WRITE);
                return FALSE;
            case EINTR:
                break;
            case EPIPE:
                snd_disconnect_channel(channel);
                return FALSE;
            default:
                red_printf("%s", strerror(errno));
                snd_disconnect_channel(channel);
                return FALSE;
            }
        } else {
            channel->send_data.pos += n;
        }
        n = channel->send_data.size - channel->send_data.pos;
    }
    return TRUE;
}

static int snd_record_handle_write(RecordChannel *record_channel, size_t size, void *message)
{
    SpiceMsgcRecordPacket *packet;
    uint32_t write_pos;
    uint32_t* data;
    uint32_t len;
    uint32_t now;

    if (!record_channel) {
        return FALSE;
    }

    packet = (SpiceMsgcRecordPacket *)message;
    size = packet->data_size;

    if (record_channel->mode == SPICE_AUDIO_DATA_MODE_CELT_0_5_1) {
        int celt_err = celt051_decode(record_channel->celt_decoder, packet->data, size,
                                      (celt_int16_t *)record_channel->celt_buf);
        if (celt_err != CELT_OK) {
            red_printf("celt decode failed (%d)", celt_err);
            return FALSE;
        }
        data = record_channel->celt_buf;
        size = FRAME_SIZE;
    } else if (record_channel->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        data = (uint32_t *)packet->data;
        size = size >> 2;
        size = MIN(size, RECORD_SAMPLES_SIZE);
    } else {
        return FALSE;
    }

    write_pos = record_channel->write_pos % RECORD_SAMPLES_SIZE;
    record_channel->write_pos += size;
    len = RECORD_SAMPLES_SIZE - write_pos;
    now = MIN(len, size);
    size -= now;
    memcpy(record_channel->samples + write_pos, data, now << 2);

    if (size) {
        memcpy(record_channel->samples, data + now, size << 2);
    }

    if (record_channel->write_pos - record_channel->read_pos > RECORD_SAMPLES_SIZE) {
        record_channel->read_pos = record_channel->write_pos - RECORD_SAMPLES_SIZE;
    }
    return TRUE;
}

static int snd_playback_handle_message(SndChannel *channel, size_t size, uint32_t type, void *message)
{
    if (!channel) {
        return FALSE;
    }

    switch (type) {
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        red_printf("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

static int snd_record_handle_message(SndChannel *channel, size_t size, uint32_t type, void *message)
{
    RecordChannel *record_channel = (RecordChannel *)channel;

    if (!channel) {
        return FALSE;
    }
    switch (type) {
    case SPICE_MSGC_RECORD_DATA:
        return snd_record_handle_write((RecordChannel *)channel, size, message);
    case SPICE_MSGC_RECORD_MODE: {
        SpiceMsgcRecordMode *mode = (SpiceMsgcRecordMode *)message;
        record_channel->mode = mode->mode;
        record_channel->mode_time = mode->time;
        if (record_channel->mode != SPICE_AUDIO_DATA_MODE_CELT_0_5_1 &&
                                                  record_channel->mode != SPICE_AUDIO_DATA_MODE_RAW) {
            red_printf("unsupported mode");
        }
        break;
    }
    case SPICE_MSGC_RECORD_START_MARK: {
        SpiceMsgcRecordStartMark *mark = (SpiceMsgcRecordStartMark *)message;
        record_channel->start_time = mark->time;
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    case SPICE_MSGC_MIGRATE_DATA: {
        RecordMigrateData* mig_data = (RecordMigrateData *)message;
        if (mig_data->version != RECORD_MIG_VERSION) {
            red_printf("invalid mig version");
            break;
        }
        record_channel->mode = mig_data->mode;
        record_channel->mode_time = mig_data->mode_time;
        record_channel->start_time = mig_data->start_time;
        break;
    }
    default:
        red_printf("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

static void snd_receive(void* data)
{
    SndChannel *channel = (SndChannel*)data;
    if (!channel) {
        return;
    }

    for (;;) {
        ssize_t n;
        n = channel->recive_data.end - channel->recive_data.now;
        ASSERT(n);
        n = reds_stream_read(channel->stream, channel->recive_data.now, n);
        if (n <= 0) {
            if (n == 0) {
                snd_disconnect_channel(channel);
                return;
            }
            ASSERT(n == -1);
            switch (errno) {
            case EAGAIN:
                return;
            case EINTR:
                break;
            case EPIPE:
                snd_disconnect_channel(channel);
                return;
            default:
                red_printf("%s", strerror(errno));
                snd_disconnect_channel(channel);
                return;
            }
        } else {
            channel->recive_data.now += n;
            for (;;) {
                SpiceDataHeader *header = channel->recive_data.message;
                uint8_t *data = (uint8_t *)(header+1);
                size_t parsed_size;
                uint8_t *parsed;
                message_destructor_t parsed_free;

                n = channel->recive_data.now - (uint8_t *)header;
                if (n < sizeof(SpiceDataHeader) || n < sizeof(SpiceDataHeader) + header->size) {
                    break;
                }
                parsed = channel->parser((void *)data, data + header->size, header->type,
                                         SPICE_VERSION_MINOR, &parsed_size, &parsed_free);
                if (parsed == NULL) {
                    red_printf("failed to parse message type %d", header->type);
                    snd_disconnect_channel(channel);
                    return;
                }
                if (!channel->handle_message(channel, parsed_size, header->type, parsed)) {
                    free(parsed);
                    snd_disconnect_channel(channel);
                    return;
                }
                parsed_free(parsed);
                channel->recive_data.message = (SpiceDataHeader *)((uint8_t *)header +
                                                                 sizeof(SpiceDataHeader) +
                                                                 header->size);
            }
            if (channel->recive_data.now == (uint8_t *)channel->recive_data.message) {
                channel->recive_data.now = channel->recive_data.buf;
                channel->recive_data.message = (SpiceDataHeader *)channel->recive_data.buf;
            } else if (channel->recive_data.now == channel->recive_data.end) {
                memcpy(channel->recive_data.buf, channel->recive_data.message, n);
                channel->recive_data.now = channel->recive_data.buf + n;
                channel->recive_data.message = (SpiceDataHeader *)channel->recive_data.buf;
            }
        }
    }
}

static void snd_event(int fd, int event, void *data)
{
    SndChannel *channel = data;

    if (event & SPICE_WATCH_EVENT_READ) {
        snd_receive(channel);
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        channel->send_messages(channel);
    }
}

static inline int snd_reset_send_data(SndChannel *channel, uint16_t verb)
{
    if (!channel) {
        return FALSE;
    }

    spice_marshaller_reset(channel->send_data.marshaller);
    channel->send_data.header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(channel->send_data.marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(channel->send_data.marshaller, sizeof(SpiceDataHeader));
    channel->send_data.pos = 0;
    channel->send_data.header->sub_list = 0;
    channel->send_data.header->size = 0;
    channel->send_data.header->type = verb;
    channel->send_data.header->serial = ++channel->send_data.serial;
    return TRUE;
}

static int snd_begin_send_message(SndChannel *channel)
{
    spice_marshaller_flush(channel->send_data.marshaller);
    channel->send_data.size = spice_marshaller_get_total_size(channel->send_data.marshaller);
    channel->send_data.header->size = channel->send_data.size - sizeof(SpiceDataHeader);
    channel->send_data.header = NULL; /* avoid writing to this until we have a new message */
    return snd_send_data(channel);
}


static int snd_playback_send_migrate(PlaybackChannel *channel)
{
    SpiceMsgMigrate migrate;

    if (!snd_reset_send_data((SndChannel *)channel, SPICE_MSG_MIGRATE)) {
        return FALSE;
    }
    migrate.flags = 0;
    spice_marshall_msg_migrate(channel->base.send_data.marshaller, &migrate);

    return snd_begin_send_message((SndChannel *)channel);
}

static int snd_playback_send_start(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    SpiceMsgPlaybackStart start;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_START)) {
        return FALSE;
    }

    start.channels = SPICE_INTERFACE_PLAYBACK_CHAN;
    start.frequency = SPICE_INTERFACE_PLAYBACK_FREQ;
    ASSERT(SPICE_INTERFACE_PLAYBACK_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    start.time = reds_get_mm_time();
    spice_marshall_msg_playback_start(channel->send_data.marshaller, &start);

    return snd_begin_send_message(channel);
}

static int snd_playback_send_stop(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_STOP)) {
        return FALSE;
    }

    return snd_begin_send_message(channel);
}

static int snd_playback_send_ctl(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;

    if ((channel->client_active = channel->active)) {
        return snd_playback_send_start(playback_channel);
    } else {
        return snd_playback_send_stop(playback_channel);
    }
}

static int snd_record_send_start(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;
    SpiceMsgRecordStart start;

    if (!snd_reset_send_data(channel, SPICE_MSG_RECORD_START)) {
        return FALSE;
    }

    start.channels = SPICE_INTERFACE_RECORD_CHAN;
    start.frequency = SPICE_INTERFACE_RECORD_FREQ;
    ASSERT(SPICE_INTERFACE_RECORD_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    spice_marshall_msg_record_start(channel->send_data.marshaller, &start);

    return snd_begin_send_message(channel);
}

static int snd_record_send_stop(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;

    if (!snd_reset_send_data(channel, SPICE_MSG_RECORD_STOP)) {
        return FALSE;
    }

    return snd_begin_send_message(channel);
}

static int snd_record_send_ctl(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;

    if ((channel->client_active = channel->active)) {
        return snd_record_send_start(record_channel);
    } else {
        return snd_record_send_stop(record_channel);
    }
}

static int snd_record_send_migrate(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;
    SpiceMsgMigrate migrate;
    SpiceDataHeader *header;
    RecordMigrateData *data;

    if (!snd_reset_send_data(channel, SPICE_MSG_MIGRATE)) {
        return FALSE;
    }

    migrate.flags = SPICE_MIGRATE_NEED_DATA_TRANSFER;
    spice_marshall_msg_migrate(channel->send_data.marshaller, &migrate);

    header = (SpiceDataHeader *)spice_marshaller_reserve_space(channel->send_data.marshaller,
                                                               sizeof(SpiceDataHeader));
    header->type = SPICE_MSG_MIGRATE_DATA;
    header->size = sizeof(RecordMigrateData);
    header->serial = ++channel->send_data.serial;
    header->sub_list = 0;

    data = (RecordMigrateData *)spice_marshaller_reserve_space(channel->send_data.marshaller,
                                                               sizeof(RecordMigrateData));
    data->version = RECORD_MIG_VERSION;
    data->serial = channel->send_data.serial;
    data->start_time = record_channel->start_time;
    data->mode = record_channel->mode;
    data->mode_time = record_channel->mode_time;

    channel->send_data.size = spice_marshaller_get_total_size(channel->send_data.marshaller);
    channel->send_data.header->size = channel->send_data.size - sizeof(SpiceDataHeader) - sizeof(SpiceDataHeader) - sizeof(*data);

    return snd_send_data(channel);
}

static int snd_playback_send_write(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    AudioFrame *frame;
    SpiceMsgPlaybackPacket msg;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_DATA)) {
        return FALSE;
    }

    frame = playback_channel->in_progress;
    msg.time = frame->time;

    spice_marshall_msg_playback_data(channel->send_data.marshaller, &msg);

    if (playback_channel->mode == SPICE_AUDIO_DATA_MODE_CELT_0_5_1) {
        int n = celt051_encode(playback_channel->celt_encoder, (celt_int16_t *)frame->samples, NULL,
                               playback_channel->send_data.celt_buf, CELT_COMPRESSED_FRAME_BYTES);
        if (n < 0) {
            red_printf("celt encode failed");
            snd_disconnect_channel(channel);
            return FALSE;
        }
        spice_marshaller_add_ref(channel->send_data.marshaller,
                                 playback_channel->send_data.celt_buf, n);
    } else {
        spice_marshaller_add_ref(channel->send_data.marshaller,
                                 (uint8_t *)frame->samples, sizeof(frame->samples));
    }

    return snd_begin_send_message(channel);
}

static int playback_send_mode(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    SpiceMsgPlaybackMode mode;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_MODE)) {
        return FALSE;
    }
    mode.time = reds_get_mm_time();
    mode.mode = playback_channel->mode;
    spice_marshall_msg_playback_mode(channel->send_data.marshaller, &mode);

    return snd_begin_send_message(channel);
}

static void snd_playback_send(void* data)
{
    PlaybackChannel *playback_channel = (PlaybackChannel*)data;
    SndChannel *channel = (SndChannel*)playback_channel;

    if (!playback_channel || !snd_send_data(data)) {
        return;
    }

    while (channel->command) {
        if (channel->command & SND_PLAYBACK_MODE_MASK) {
            if (!playback_send_mode(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_MODE_MASK;
        }
        if (channel->command & SND_PLAYBACK_PCM_MASK) {
            ASSERT(!playback_channel->in_progress && playback_channel->pending_frame);
            playback_channel->in_progress = playback_channel->pending_frame;
            playback_channel->pending_frame = NULL;
            channel->command &= ~SND_PLAYBACK_PCM_MASK;
            if (!snd_playback_send_write(playback_channel)) {
                red_printf("snd_send_playback_write failed");
                return;
            }
        }
        if (channel->command & SND_PLAYBACK_CTRL_MASK) {
            if (!snd_playback_send_ctl(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_CTRL_MASK;
        }
        if (channel->command & SND_PLAYBACK_MIGRATE_MASK) {
            if (!snd_playback_send_migrate(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_MIGRATE_MASK;
        }
    }
}

static void snd_record_send(void* data)
{
    RecordChannel *record_channel = (RecordChannel*)data;
    SndChannel *channel = (SndChannel*)record_channel;

    if (!record_channel || !snd_send_data(data)) {
        return;
    }

    while (channel->command) {
        if (channel->command & SND_RECORD_CTRL_MASK) {
            if (!snd_record_send_ctl(record_channel)) {
                return;
            }
            channel->command &= ~SND_RECORD_CTRL_MASK;
        }
        if (channel->command & SND_RECORD_MIGRATE_MASK) {
            if (!snd_record_send_migrate(record_channel)) {
                return;
            }
            channel->command &= ~SND_RECORD_MIGRATE_MASK;
        }
    }
}

static SndChannel *__new_channel(SndWorker *worker, int size, uint32_t channel_id,
                                 RedsStream *stream,
                                 int migrate, send_messages_proc send_messages,
                                 handle_message_proc handle_message,
                                 on_message_done_proc on_message_done,
                                 cleanup_channel_proc cleanup)
{
    SndChannel *channel;
    int delay_val;
    int flags;
    int priority;
    int tos;

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error1;
    }

    priority = 6;
    if (setsockopt(stream->socket, SOL_SOCKET, SO_PRIORITY, (void*)&priority,
                   sizeof(priority)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    tos = IPTOS_LOWDELAY;
    if (setsockopt(stream->socket, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    delay_val = IS_LOW_BANDWIDTH() ? 0 : 1;
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error1;
    }

    ASSERT(size >= sizeof(*channel));
    channel = spice_malloc0(size);
    channel->parser = spice_get_client_channel_parser(channel_id, NULL);
    channel->stream = stream;
    channel->worker = worker;
    channel->recive_data.message = (SpiceDataHeader *)channel->recive_data.buf;
    channel->recive_data.now = channel->recive_data.buf;
    channel->recive_data.end = channel->recive_data.buf + sizeof(channel->recive_data.buf);
    channel->send_data.marshaller = spice_marshaller_new();

    stream->watch = core->watch_add(stream->socket, SPICE_WATCH_EVENT_READ,
                                  snd_event, channel);
    if (stream->watch == NULL) {
        red_printf("watch_add failed, %s", strerror(errno));
        goto error2;
    }

    channel->migrate = migrate;
    channel->send_messages = send_messages;
    channel->handle_message = handle_message;
    channel->on_message_done = on_message_done;
    channel->cleanup = cleanup;
    return channel;

error2:
    free(channel);

error1:
    reds_stream_free(stream);
    return NULL;
}

static void snd_shutdown(Channel *channel)
{
    SndWorker *worker = (SndWorker *)channel;
    snd_disconnect_channel(worker->connection);
}

static void snd_set_command(SndChannel *channel, uint32_t command)
{
    if (!channel) {
        return;
    }
    channel->command |= command;
}

SPICE_GNUC_VISIBLE void spice_server_playback_start(SpicePlaybackInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);

    sin->st->worker.active = 1;
    if (!channel)
        return;
    ASSERT(!playback_channel->base.active);
    reds_desable_mm_timer();
    playback_channel->base.active = TRUE;
    if (!playback_channel->base.client_active) {
        snd_set_command(&playback_channel->base, SND_PLAYBACK_CTRL_MASK);
        snd_playback_send(&playback_channel->base);
    } else {
        playback_channel->base.command &= ~SND_PLAYBACK_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_stop(SpicePlaybackInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);

    sin->st->worker.active = 0;
    if (!channel)
        return;
    ASSERT(playback_channel->base.active);
    reds_enable_mm_timer();
    playback_channel->base.active = FALSE;
    if (playback_channel->base.client_active) {
        snd_set_command(&playback_channel->base, SND_PLAYBACK_CTRL_MASK);
        snd_playback_send(&playback_channel->base);
    } else {
        playback_channel->base.command &= ~SND_PLAYBACK_CTRL_MASK;
        playback_channel->base.command &= ~SND_PLAYBACK_PCM_MASK;

        if (playback_channel->pending_frame) {
            ASSERT(!playback_channel->in_progress);
            snd_playback_free_frame(playback_channel,
                                    playback_channel->pending_frame);
            playback_channel->pending_frame = NULL;
        }
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_get_buffer(SpicePlaybackInstance *sin,
                                                         uint32_t **frame, uint32_t *num_samples)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);

    if (!channel || !playback_channel->free_frames) {
        *frame = NULL;
        *num_samples = 0;
        return;
    }
    ASSERT(playback_channel->base.active);

    *frame = playback_channel->free_frames->samples;
    playback_channel->free_frames = playback_channel->free_frames->next;
    *num_samples = FRAME_SIZE;
}

SPICE_GNUC_VISIBLE void spice_server_playback_put_samples(SpicePlaybackInstance *sin, uint32_t *samples)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);
    AudioFrame *frame;

    if (!channel)
        return;
    ASSERT(playback_channel->base.active);

    if (playback_channel->pending_frame) {
        snd_playback_free_frame(playback_channel, playback_channel->pending_frame);
    }
    frame = SPICE_CONTAINEROF(samples, AudioFrame, samples);
    frame->time = reds_get_mm_time();
    red_dispatcher_set_mm_time(frame->time);
    playback_channel->pending_frame = frame;
    snd_set_command(&playback_channel->base, SND_PLAYBACK_PCM_MASK);
    snd_playback_send(&playback_channel->base);
}

static void on_new_playback_channel(SndWorker *worker)
{
    PlaybackChannel *playback_channel =
        SPICE_CONTAINEROF(worker->connection, PlaybackChannel, base);

    ASSERT(playback_channel);

    snd_set_command((SndChannel *)playback_channel, SND_PLAYBACK_MODE_MASK);
    if (!playback_channel->base.migrate && playback_channel->base.active) {
        snd_set_command((SndChannel *)playback_channel, SND_PLAYBACK_CTRL_MASK);
    }
    if (playback_channel->base.active) {
        reds_desable_mm_timer();
    }
}

static void snd_playback_cleanup(SndChannel *channel)
{
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);

    if (playback_channel->base.active) {
        reds_enable_mm_timer();
    }

    celt051_encoder_destroy(playback_channel->celt_encoder);
    celt051_mode_destroy(playback_channel->celt_mode);
}

static void snd_set_playback_peer(Channel *channel, RedsStream *stream, int migration,
                                  int num_common_caps, uint32_t *common_caps, int num_caps,
                                  uint32_t *caps)
{
    SndWorker *worker = (SndWorker *)channel;
    SpicePlaybackState *st = SPICE_CONTAINEROF(worker, SpicePlaybackState, worker);
    PlaybackChannel *playback_channel;
    CELTEncoder *celt_encoder;
    CELTMode *celt_mode;
    int celt_error;

    snd_disconnect_channel(worker->connection);

    if (!(celt_mode = celt051_mode_create(SPICE_INTERFACE_PLAYBACK_FREQ,
                                          SPICE_INTERFACE_PLAYBACK_CHAN,
                                          FRAME_SIZE, &celt_error))) {
        red_printf("create celt mode failed %d", celt_error);
        return;
    }

    if (!(celt_encoder = celt051_encoder_create(celt_mode))) {
        red_printf("create celt encoder failed");
        goto error_1;
    }

    if (!(playback_channel = (PlaybackChannel *)__new_channel(worker,
                                                              sizeof(*playback_channel),
                                                              SPICE_CHANNEL_PLAYBACK,
                                                              stream,
                                                              migration,
                                                              snd_playback_send,
                                                              snd_playback_handle_message,
                                                              snd_playback_on_message_done,
                                                              snd_playback_cleanup))) {
        goto error_2;
    }
    worker->connection = &playback_channel->base;
    snd_playback_free_frame(playback_channel, &playback_channel->frames[0]);
    snd_playback_free_frame(playback_channel, &playback_channel->frames[1]);
    snd_playback_free_frame(playback_channel, &playback_channel->frames[2]);

    playback_channel->celt_mode = celt_mode;
    playback_channel->celt_encoder = celt_encoder;
    playback_channel->celt_allowed = num_caps > 0 && (caps[0] & (1 << SPICE_PLAYBACK_CAP_CELT_0_5_1));
    playback_channel->mode = playback_channel->celt_allowed ? playback_compression :
                                                              SPICE_AUDIO_DATA_MODE_RAW;

    on_new_playback_channel(worker);
    if (worker->active) {
        spice_server_playback_start(st->sin);
    }
    snd_playback_send(worker->connection);
    return;

error_2:
    celt051_encoder_destroy(celt_encoder);

error_1:
    celt051_mode_destroy(celt_mode);
}

static void snd_record_migrate(Channel *channel)
{
    SndWorker *worker = (SndWorker *)channel;
    if (worker->connection) {
        snd_set_command(worker->connection, SND_RECORD_MIGRATE_MASK);
        snd_record_send(worker->connection);
    }
}

SPICE_GNUC_VISIBLE void spice_server_record_start(SpiceRecordInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);

    sin->st->worker.active = 1;
    if (!channel)
        return;
    ASSERT(!record_channel->base.active);
    record_channel->base.active = TRUE;
    record_channel->read_pos = record_channel->write_pos = 0;   //todo: improve by
                                                                //stream generation
    if (!record_channel->base.client_active) {
        snd_set_command(&record_channel->base, SND_RECORD_CTRL_MASK);
        snd_record_send(&record_channel->base);
    } else {
        record_channel->base.command &= ~SND_RECORD_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE void spice_server_record_stop(SpiceRecordInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);

    sin->st->worker.active = 0;
    if (!channel)
        return;
    ASSERT(record_channel->base.active);
    record_channel->base.active = FALSE;
    if (record_channel->base.client_active) {
        snd_set_command(&record_channel->base, SND_RECORD_CTRL_MASK);
        snd_record_send(&record_channel->base);
    } else {
        record_channel->base.command &= ~SND_RECORD_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_record_get_samples(SpiceRecordInstance *sin,
                                                            uint32_t *samples, uint32_t bufsize)
{
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);
    uint32_t read_pos;
    uint32_t now;
    uint32_t len;

    if (!channel)
        return 0;
    ASSERT(record_channel->base.active);

    if (record_channel->write_pos < RECORD_SAMPLES_SIZE / 2) {
        return 0;
    }

    len = MIN(record_channel->write_pos - record_channel->read_pos, bufsize);

    if (len < bufsize) {
        SndWorker *worker = record_channel->base.worker;
        snd_receive(record_channel);
        if (!worker->connection) {
            return 0;
        }
        len = MIN(record_channel->write_pos - record_channel->read_pos, bufsize);
    }

    read_pos = record_channel->read_pos % RECORD_SAMPLES_SIZE;
    record_channel->read_pos += len;
    now = MIN(len, RECORD_SAMPLES_SIZE - read_pos);
    memcpy(samples, &record_channel->samples[read_pos], now * 4);
    if (now < len) {
        memcpy(samples + now, record_channel->samples, (len - now) * 4);
    }
    return len;
}

static void on_new_record_channel(SndWorker *worker)
{
    RecordChannel *record_channel = (RecordChannel *)worker->connection;
    ASSERT(record_channel);

    if (!record_channel->base.migrate) {
        if (record_channel->base.active) {
            snd_set_command((SndChannel *)record_channel, SND_RECORD_CTRL_MASK);
        }
    }
}

static void snd_record_cleanup(SndChannel *channel)
{
    RecordChannel *record_channel = (RecordChannel *)channel;

    celt051_decoder_destroy(record_channel->celt_decoder);
    celt051_mode_destroy(record_channel->celt_mode);
}

static void snd_set_record_peer(Channel *channel, RedsStream *stream, int migration,
                                int num_common_caps, uint32_t *common_caps, int num_caps,
                                uint32_t *caps)
{
    SndWorker *worker = (SndWorker *)channel;
    SpiceRecordState *st = SPICE_CONTAINEROF(worker, SpiceRecordState, worker);
    RecordChannel *record_channel;
    CELTDecoder *celt_decoder;
    CELTMode *celt_mode;
    int celt_error;

    snd_disconnect_channel(worker->connection);

    if (!(celt_mode = celt051_mode_create(SPICE_INTERFACE_RECORD_FREQ,
                                          SPICE_INTERFACE_RECORD_CHAN,
                                          FRAME_SIZE, &celt_error))) {
        red_printf("create celt mode failed %d", celt_error);
        return;
    }

    if (!(celt_decoder = celt051_decoder_create(celt_mode))) {
        red_printf("create celt decoder failed");
        goto error_1;
    }

    if (!(record_channel = (RecordChannel *)__new_channel(worker,
                                                          sizeof(*record_channel),
                                                          SPICE_CHANNEL_RECORD,
                                                          stream,
                                                          migration,
                                                          snd_record_send,
                                                          snd_record_handle_message,
                                                          snd_record_on_message_done,
                                                          snd_record_cleanup))) {
        goto error_2;
    }

    worker->connection = &record_channel->base;

    record_channel->celt_mode = celt_mode;
    record_channel->celt_decoder = celt_decoder;

    on_new_record_channel(worker);
    if (worker->active) {
        spice_server_record_start(st->sin);
    }
    snd_record_send(worker->connection);
    return;

error_1:
    celt051_decoder_destroy(celt_decoder);

error_2:
    celt051_mode_destroy(celt_mode);
}

static void snd_playback_migrate(Channel *channel)
{
    SndWorker *worker = (SndWorker *)channel;

    if (worker->connection) {
        snd_set_command(worker->connection, SND_PLAYBACK_MIGRATE_MASK);
        snd_playback_send(worker->connection);
    }
}

static void add_worker(SndWorker *worker)
{
    worker->next = workers;
    workers = worker;
}

static void remove_worker(SndWorker *worker)
{
    SndWorker **now = &workers;
    while (*now) {
        if (*now == worker) {
            *now = worker->next;
            return;
        }
        now = &(*now)->next;
    }
    red_printf("not found");
}

void snd_attach_playback(SpicePlaybackInstance *sin)
{
    SndWorker *playback_worker;

    sin->st = spice_new0(SpicePlaybackState, 1);
    sin->st->sin = sin;
    playback_worker = &sin->st->worker;

    playback_worker->base.type = SPICE_CHANNEL_PLAYBACK;
    playback_worker->base.link = snd_set_playback_peer;
    playback_worker->base.shutdown = snd_shutdown;
    playback_worker->base.migrate = snd_playback_migrate;
    playback_worker->base.data = NULL;

    playback_worker->base.num_caps = 1;
    playback_worker->base.caps = spice_new(uint32_t, 1);
    playback_worker->base.caps[0] = (1 << SPICE_PLAYBACK_CAP_CELT_0_5_1);

    add_worker(playback_worker);
    reds_register_channel(&playback_worker->base);
}

void snd_attach_record(SpiceRecordInstance *sin)
{
    SndWorker *record_worker;

    sin->st = spice_new0(SpiceRecordState, 1);
    sin->st->sin = sin;
    record_worker = &sin->st->worker;

    record_worker->base.type = SPICE_CHANNEL_RECORD;
    record_worker->base.link = snd_set_record_peer;
    record_worker->base.shutdown = snd_shutdown;
    record_worker->base.migrate = snd_record_migrate;
    record_worker->base.data = NULL;

    record_worker->base.num_caps = 1;
    record_worker->base.caps = spice_new(uint32_t, 1);
    record_worker->base.caps[0] = (1 << SPICE_RECORD_CAP_CELT_0_5_1);
    add_worker(record_worker);
    reds_register_channel(&record_worker->base);
}

static void snd_detach_common(SndWorker *worker)
{
    if (!worker) {
        return;
    }
    remove_worker(worker);
    snd_disconnect_channel(worker->connection);
    reds_unregister_channel(&worker->base);

    reds_channel_dispose(&worker->base);
}

void snd_detach_playback(SpicePlaybackInstance *sin)
{
    snd_detach_common(&sin->st->worker);
    free(sin->st);
}

void snd_detach_record(SpiceRecordInstance *sin)
{
    snd_detach_common(&sin->st->worker);
    free(sin->st);
}

void snd_set_playback_compression(int on)
{
    SndWorker *now = workers;

    playback_compression = on ? SPICE_AUDIO_DATA_MODE_CELT_0_5_1 : SPICE_AUDIO_DATA_MODE_RAW;
    for (; now; now = now->next) {
        if (now->base.type == SPICE_CHANNEL_PLAYBACK && now->connection) {
            PlaybackChannel* playback = (PlaybackChannel*)now->connection;
            if (!playback->celt_allowed) {
                ASSERT(playback->mode == SPICE_AUDIO_DATA_MODE_RAW);
                continue;
            }
            if (playback->mode != playback_compression) {
                playback->mode = playback_compression;
                snd_set_command(now->connection, SND_PLAYBACK_MODE_MASK);
            }
        }
    }
}

int snd_get_playback_compression(void)
{
    return (playback_compression == SPICE_AUDIO_DATA_MODE_RAW) ? FALSE : TRUE;
}

