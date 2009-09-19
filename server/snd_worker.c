/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <celt051/celt.h>

#include "vd_interface.h"
#include "red_common.h"
#include "reds.h"
#include "red_dispatcher.h"
#include "snd_worker.h"

#define MAX_SEND_VEC 100
#define MAX_SEND_BUFS 200

#define RECIVE_BUF_SIZE (16 * 1024 * 2)

#define FRAME_SIZE 256
#define PLAYBACK_BUF_SIZE (FRAME_SIZE * 4)

#define CELT_BIT_RATE (64 * 1024)
#define CELT_COMPRESSED_FRAME_BYTES (FRAME_SIZE * CELT_BIT_RATE / VD_INTERFACE_PLAYBACK_FREQ / 8)

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

typedef struct BufDescriptor {
    uint32_t size;
    uint8_t *data;
} BufDescriptor;

typedef struct SndChannel SndChannel;
typedef void (*send_messages_proc)(void *in_channel);
typedef int (*handle_message_proc)(SndChannel *channel, RedDataHeader *message);
typedef void (*on_message_done_proc)(SndChannel *channel);
typedef void (*cleanup_channel_proc)(SndChannel *channel);

typedef struct SndWorker SndWorker;

struct SndChannel {
    RedsStreamContext *peer;
    SndWorker *worker;

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
        RedDataHeader header;
        uint32_t n_bufs;
        BufDescriptor bufs[MAX_SEND_BUFS];

        uint32_t size;
        uint32_t pos;
    } send_data;

    struct {
        uint8_t buf[RECIVE_BUF_SIZE];
        RedDataHeader *message;
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
    PlaybackPlug plug;
    VDObjectRef plug_ref;
    AudioFrame *free_frames;
    AudioFrame *in_progress;
    AudioFrame *pending_frame;
    CELTMode *celt_mode;
    CELTEncoder *celt_encoder;
    int celt_allowed;
    uint32_t mode;
    struct {
        union {
            RedPlaybackMode mode;
            RedPlaybackStart start;
            RedMigrate migrate;
            uint8_t celt_buf[CELT_COMPRESSED_FRAME_BYTES];
        } u;
    } send_data;
} PlaybackChannel;

struct SndWorker {
    Channel base;
    VDInterface *interface;
    SndChannel *connection;
    SndWorker *next;
};

#define RECORD_MIG_VERSION 1

typedef struct __attribute__ ((__packed__)) RecordMigrateData {
    uint32_t version;
    uint64_t serial;
    uint32_t start_time;
    uint32_t mode;
    uint32_t mode_time;
} RecordMigrateData;

typedef struct __attribute__ ((__packed__)) RecordMigrateMessage {
    RedMigrate migrate;
    RedDataHeader header;
    RecordMigrateData data;
} RecordMigrateMessage;

typedef struct RecordChannel {
    SndChannel base;
    RecordPlug plug;
    VDObjectRef plug_ref;
    uint32_t samples[RECORD_SAMPLES_SIZE];
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t mode;
    uint32_t mode_time;
    uint32_t start_time;
    CELTDecoder *celt_decoder;
    CELTMode *celt_mode;
    uint32_t celt_buf[FRAME_SIZE];
    struct {
        union {
            RedRecordStart start;
            RecordMigrateMessage migrate;
        } u;
    } send_data;
} RecordChannel;

static SndWorker *workers = NULL;
static uint32_t playback_compression = RED_AUDIO_DATA_MODE_CELT_0_5_1;

static void snd_receive(void* data);

static inline BufDescriptor *snd_find_buf(SndChannel *channel, int buf_pos, int *buf_offset)
{
    BufDescriptor *buf;
    int pos = 0;

    for (buf = channel->send_data.bufs; buf_pos >= pos + buf->size; buf++) {
        pos += buf->size;
        ASSERT(buf != &channel->send_data.bufs[channel->send_data.n_bufs - 1]);
    }
    *buf_offset = buf_pos - pos;
    return buf;
}

static inline uint32_t __snd_fill_iovec(BufDescriptor *buf, int skip, struct iovec *vec,
                                        int *vec_index, long phys_delta)
{
    uint32_t size = 0;
    vec[*vec_index].iov_base = buf->data + skip;
    vec[*vec_index].iov_len = size = buf->size - skip;
    (*vec_index)++;
    return size;
}

static inline void snd_fill_iovec(SndChannel *channel, struct iovec *vec, int *vec_size)
{
    int vec_index = 0;
    uint32_t pos = channel->send_data.pos;
    ASSERT(channel->send_data.size != pos && channel->send_data.size > pos);

    do {
        BufDescriptor *buf;
        int buf_offset;

        buf = snd_find_buf(channel, pos, &buf_offset);
        ASSERT(buf);
        pos += __snd_fill_iovec(buf, buf_offset, vec, &vec_index, 0);
    } while (vec_index < MAX_SEND_VEC && pos != channel->send_data.size);
    *vec_size = vec_index;
}

static void snd_disconnect_channel(SndChannel *channel)
{
    SndWorker *worker;

    if (!channel) {
        return;
    }
    channel->cleanup(channel);
    worker = channel->worker;
    worker->connection = NULL;
    core->set_file_handlers(core, channel->peer->socket, NULL, NULL, NULL);
    channel->peer->cb_free(channel->peer);
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
                if (core->set_file_handlers(core, channel->peer->socket, snd_receive,
                                            NULL, channel) == -1) {
                    red_printf("qemu_set_fd_handler failed");
                }
            }
            break;
        }

        snd_fill_iovec(channel, vec, &vec_size);
        if ((n = channel->peer->cb_writev(channel->peer->ctx, vec, vec_size)) == -1) {
            switch (errno) {
            case EAGAIN:
                channel->blocked = TRUE;
                if (core->set_file_handlers(core, channel->peer->socket, snd_receive,
                                            channel->send_messages, channel) == -1) {
                    red_printf("qemu_set_fd_handler failed");
                }
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

static int snd_record_handle_write(RecordChannel *record_channel, RedDataHeader *message)
{
    RedcRecordPacket *packet;
    uint32_t write_pos;
    uint32_t* data;
    uint32_t size;
    uint32_t len;
    uint32_t now;

    if (!record_channel) {
        return FALSE;
    }

    packet = (RedcRecordPacket *)(message + 1);
    size = message->size - sizeof(*packet);

    if (record_channel->mode == RED_AUDIO_DATA_MODE_CELT_0_5_1) {
        int celt_err = celt051_decode(record_channel->celt_decoder, packet->data, size,
                                      (celt_int16_t *)record_channel->celt_buf);
        if (celt_err != CELT_OK) {
            red_printf("celt decode failed (%d)", celt_err);
            return FALSE;
        }
        data = record_channel->celt_buf;
        size = FRAME_SIZE;
    } else if (record_channel->mode == RED_AUDIO_DATA_MODE_RAW) {
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

static int snd_playback_handle_message(SndChannel *channel, RedDataHeader *message)
{
    if (!channel) {
        return FALSE;
    }

    switch (message->type) {
    case REDC_DISCONNECTING:
        break;
    default:
        red_printf("invalid message type %u", message->type);
        return FALSE;
    }
    return TRUE;
}

static int snd_record_handle_message(SndChannel *channel, RedDataHeader *message)
{
    RecordChannel *record_channel = (RecordChannel *)channel;

    if (!channel) {
        return FALSE;
    }
    switch (message->type) {
    case REDC_RECORD_DATA:
        return snd_record_handle_write((RecordChannel *)channel, message);
    case REDC_RECORD_MODE: {
        RedcRecordMode *mode = (RedcRecordMode *)(message + 1);
        record_channel->mode = mode->mode;
        record_channel->mode_time = mode->time;
        if (record_channel->mode != RED_AUDIO_DATA_MODE_CELT_0_5_1 &&
                                                  record_channel->mode != RED_AUDIO_DATA_MODE_RAW) {
            red_printf("unsupported mode");
        }
        break;
    }
    case REDC_RECORD_START_MARK: {
        RedcRecordStartMark *mark = (RedcRecordStartMark *)(message + 1);
        record_channel->start_time = mark->time;
        break;
    }
    case REDC_DISCONNECTING:
        break;
    case REDC_MIGRATE_DATA: {
        RecordMigrateData* mig_data = (RecordMigrateData *)(message + 1);
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
        red_printf("invalid message type %u", message->type);
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
        if ((n = channel->peer->cb_read(channel->peer->ctx, channel->recive_data.now, n)) <= 0) {
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
                RedDataHeader *message = channel->recive_data.message;
                n = channel->recive_data.now - (uint8_t *)message;
                if (n < sizeof(RedDataHeader) || n < sizeof(RedDataHeader) + message->size) {
                    break;
                }
                if (!channel->handle_message(channel, message)) {
                    snd_disconnect_channel(channel);
                    return;
                }
                channel->recive_data.message = (RedDataHeader *)((uint8_t *)message +
                                                                 sizeof(RedDataHeader) +
                                                                 message->size);
            }
            if (channel->recive_data.now == (uint8_t *)channel->recive_data.message) {
                channel->recive_data.now = channel->recive_data.buf;
                channel->recive_data.message = (RedDataHeader *)channel->recive_data.buf;
            } else if (channel->recive_data.now == channel->recive_data.end) {
                memcpy(channel->recive_data.buf, channel->recive_data.message, n);
                channel->recive_data.now = channel->recive_data.buf + n;
                channel->recive_data.message = (RedDataHeader *)channel->recive_data.buf;
            }
        }
    }
}

static inline void __snd_add_buf(SndChannel *channel, void *data, uint32_t size)
{
    int pos = channel->send_data.n_bufs++;
    ASSERT(pos < MAX_SEND_BUFS);
    channel->send_data.bufs[pos].size = size;
    channel->send_data.bufs[pos].data = data;
}

static void snd_add_buf(SndChannel *channel, void *data, uint32_t size)
{
    __snd_add_buf(channel, data, size);
    channel->send_data.header.size += size;
}

static inline int snd_reset_send_data(SndChannel *channel, uint16_t verb)
{
    if (!channel) {
        return FALSE;
    }

    channel->send_data.pos = 0;
    channel->send_data.n_bufs = 0;
    channel->send_data.header.sub_list = 0;
    channel->send_data.header.size = 0;
    channel->send_data.header.type = verb;
    ++channel->send_data.header.serial;
    __snd_add_buf(channel, &channel->send_data.header, sizeof(RedDataHeader));
    return TRUE;
}

static int snd_playback_send_migrate(PlaybackChannel *channel)
{
    if (!snd_reset_send_data((SndChannel *)channel, RED_MIGRATE)) {
        return FALSE;
    }
    channel->send_data.u.migrate.flags = 0;
    snd_add_buf((SndChannel *)channel, &channel->send_data.u.migrate,
                sizeof(channel->send_data.u.migrate));
    channel->base.send_data.size = channel->base.send_data.header.size + sizeof(RedDataHeader);
    return snd_send_data((SndChannel *)channel);
}

static int snd_playback_send_start(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    RedPlaybackStart *start;
    if (!snd_reset_send_data(channel, RED_PLAYBACK_START)) {
        return FALSE;
    }

    start = &playback_channel->send_data.u.start;
    start->channels = VD_INTERFACE_PLAYBACK_CHAN;
    start->frequency = VD_INTERFACE_PLAYBACK_FREQ;
    ASSERT(VD_INTERFACE_PLAYBACK_FMT == VD_INTERFACE_AUDIO_FMT_S16);
    start->format = RED_AUDIO_FMT_S16;
    start->time = reds_get_mm_time();
    snd_add_buf(channel, start, sizeof(*start));

    channel->send_data.size = sizeof(RedDataHeader) + sizeof(*start);
    return snd_send_data(channel);
}

static int snd_playback_send_stop(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    if (!snd_reset_send_data(channel, RED_PLAYBACK_STOP)) {
        return FALSE;
    }
    channel->send_data.size = sizeof(RedDataHeader);
    return snd_send_data(channel);
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
    RedRecordStart *start;
    if (!snd_reset_send_data(channel, RED_RECORD_START)) {
        return FALSE;
    }

    start = &record_channel->send_data.u.start;
    start->channels = VD_INTERFACE_RECORD_CHAN;
    start->frequency = VD_INTERFACE_RECORD_FREQ;
    ASSERT(VD_INTERFACE_RECORD_FMT == VD_INTERFACE_AUDIO_FMT_S16);
    start->format = RED_AUDIO_FMT_S16;
    snd_add_buf(channel, start, sizeof(*start));

    channel->send_data.size = sizeof(RedDataHeader) + sizeof(*start);
    return snd_send_data(channel);
}

static int snd_record_send_stop(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;
    if (!snd_reset_send_data(channel, RED_RECORD_STOP)) {
        return FALSE;
    }
    channel->send_data.size = sizeof(RedDataHeader);
    return snd_send_data(channel);
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
    RecordMigrateMessage* migrate;

    if (!snd_reset_send_data(channel, RED_MIGRATE)) {
        return FALSE;
    }

    migrate = &record_channel->send_data.u.migrate;
    migrate->migrate.flags = RED_MIGRATE_NEED_DATA_TRANSFER;
    migrate->header.type = RED_MIGRATE_DATA;
    migrate->header.size = sizeof(RecordMigrateData);
    migrate->header.serial = ++channel->send_data.header.serial;
    migrate->header.sub_list = 0;

    migrate->data.version = RECORD_MIG_VERSION;
    migrate->data.serial = channel->send_data.header.serial;
    migrate->data.start_time = record_channel->start_time;
    migrate->data.mode = record_channel->mode;
    migrate->data.mode_time = record_channel->mode_time;

    snd_add_buf(channel, migrate, sizeof(*migrate));
    channel->send_data.size = channel->send_data.header.size + sizeof(RedDataHeader);
    channel->send_data.header.size -= sizeof(migrate->header);
    channel->send_data.header.size -= sizeof(migrate->data);
    return snd_send_data(channel);
}

static int snd_playback_send_write(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    AudioFrame *frame;

    if (!snd_reset_send_data(channel, RED_PLAYBACK_DATA)) {
        return FALSE;
    }

    frame = playback_channel->in_progress;
    snd_add_buf(channel, &frame->time, sizeof(frame->time));
    if (playback_channel->mode == RED_AUDIO_DATA_MODE_CELT_0_5_1) {
        int n = celt051_encode(playback_channel->celt_encoder, (celt_int16_t *)frame->samples, NULL,
                               playback_channel->send_data.u.celt_buf, CELT_COMPRESSED_FRAME_BYTES);
        if (n < 0) {
            red_printf("celt encode failed");
            snd_disconnect_channel(channel);
            return FALSE;
        }
        snd_add_buf(channel, playback_channel->send_data.u.celt_buf, n);
    } else {
        snd_add_buf(channel, frame->samples, sizeof(frame->samples));
    }

    channel->send_data.size = channel->send_data.header.size + sizeof(RedDataHeader);

    return snd_send_data(channel);
}

static int playback_send_mode(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    RedPlaybackMode *mode;

    if (!snd_reset_send_data(channel, RED_PLAYBACK_MODE)) {
        return FALSE;
    }
    mode = &playback_channel->send_data.u.mode;
    mode->time = reds_get_mm_time();
    mode->mode = playback_channel->mode;
    snd_add_buf(channel, mode, sizeof(*mode));

    channel->send_data.size = channel->send_data.header.size + sizeof(RedDataHeader);
    return snd_send_data(channel);
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

static SndChannel *__new_channel(SndWorker *worker, int size, RedsStreamContext *peer,
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

    if ((flags = fcntl(peer->socket, F_GETFL)) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error1;
    }

    priority = 6;
    if (setsockopt(peer->socket, SOL_SOCKET, SO_PRIORITY, (void*)&priority,
                   sizeof(priority)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    tos = IPTOS_LOWDELAY;
    if (setsockopt(peer->socket, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    delay_val = IS_LOW_BANDWIDTH() ? 0 : 1;
    if (setsockopt(peer->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        red_printf("setsockopt failed, %s", strerror(errno));
    }

    if (fcntl(peer->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        red_printf("accept failed, %s", strerror(errno));
        goto error1;
    }

    ASSERT(size >= sizeof(*channel));
    if (!(channel = malloc(size))) {
        red_printf("malloc failed");
        goto error1;
    }
    memset(channel, 0, size);
    channel->peer = peer;
    channel->worker = worker;
    channel->recive_data.message = (RedDataHeader *)channel->recive_data.buf;
    channel->recive_data.now = channel->recive_data.buf;
    channel->recive_data.end = channel->recive_data.buf + sizeof(channel->recive_data.buf);

    if (core->set_file_handlers(core, peer->socket, snd_receive, NULL, channel) == -1) {
        red_printf("qemu_set_fd_handler failed, %s", strerror(errno));
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
    peer->cb_free(peer);
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

static void snd_playback_start(PlaybackPlug *plug)
{
    PlaybackChannel *playback_channel = CONTAINEROF(plug, PlaybackChannel, plug);

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

static void snd_playback_stop(PlaybackPlug *plug)
{
    PlaybackChannel *playback_channel = CONTAINEROF(plug, PlaybackChannel, plug);

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

static void snd_playback_get_frame(PlaybackPlug *plug, uint32_t **frame, uint32_t *num_samples)
{
    PlaybackChannel *playback_channel = CONTAINEROF(plug, PlaybackChannel, plug);

    ASSERT(playback_channel->base.active);
    if (!playback_channel->free_frames) {
        *frame = NULL;
        *num_samples = 0;
        return;
    }

    *frame = playback_channel->free_frames->samples;
    playback_channel->free_frames = playback_channel->free_frames->next;
    *num_samples = FRAME_SIZE;
}

static void snd_playback_put_frame(PlaybackPlug *plug, uint32_t *samples)
{
    PlaybackChannel *playback_channel = CONTAINEROF(plug, PlaybackChannel, plug);
    AudioFrame *frame;

    ASSERT(playback_channel->base.active);

    if (playback_channel->pending_frame) {
        snd_playback_free_frame(playback_channel, playback_channel->pending_frame);
    }
    frame = CONTAINEROF(samples, AudioFrame, samples);
    frame->time = reds_get_mm_time();
    red_dispatcher_set_mm_time(frame->time);
    playback_channel->pending_frame = frame;
    snd_set_command(&playback_channel->base, SND_PLAYBACK_PCM_MASK);
    snd_playback_send(&playback_channel->base);
}

static void on_new_playback_channel(SndWorker *worker)
{
    PlaybackChannel *playback_channel = (PlaybackChannel *)worker->connection;
    PlaybackInterface *interface = (PlaybackInterface *)worker->interface;
    ASSERT(playback_channel);

    playback_channel->plug_ref = interface->plug(interface, &playback_channel->plug,
                                                 &playback_channel->base.active);
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
    PlaybackChannel *playback_channel = (PlaybackChannel *)channel;
    PlaybackInterface *interface = (PlaybackInterface *)channel->worker->interface;

    if (playback_channel->base.active) {
        reds_enable_mm_timer();
    }
    interface->unplug(interface, playback_channel->plug_ref);

    celt051_encoder_destroy(playback_channel->celt_encoder);
    celt051_mode_destroy(playback_channel->celt_mode);
}

static void snd_set_playback_peer(Channel *channel, RedsStreamContext *peer, int migration,
                                  int num_common_caps, uint32_t *common_caps, int num_caps,
                                  uint32_t *caps)
{
    SndWorker *worker = (SndWorker *)channel;
    PlaybackChannel *playback_channel;
    CELTEncoder *celt_encoder;
    CELTMode *celt_mode;
    int celt_error;

    snd_disconnect_channel(worker->connection);

    if (!(celt_mode = celt051_mode_create(VD_INTERFACE_PLAYBACK_FREQ, VD_INTERFACE_PLAYBACK_CHAN,
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
                                                              peer,
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

    playback_channel->plug.major_version = VD_INTERFACE_PLAYBACK_MAJOR;
    playback_channel->plug.minor_version = VD_INTERFACE_PLAYBACK_MINOR;
    playback_channel->plug.start = snd_playback_start;
    playback_channel->plug.stop = snd_playback_stop;
    playback_channel->plug.get_frame = snd_playback_get_frame;
    playback_channel->plug.put_frame = snd_playback_put_frame;
    playback_channel->celt_mode = celt_mode;
    playback_channel->celt_encoder = celt_encoder;
    playback_channel->celt_allowed = num_caps > 0 && (caps[0] & (1 << RED_PLAYBACK_CAP_CELT_0_5_1));
    playback_channel->mode = playback_channel->celt_allowed ? playback_compression :
                                                              RED_AUDIO_DATA_MODE_RAW;

    on_new_playback_channel(worker);
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

static void snd_record_start(RecordPlug *plug)
{
    RecordChannel *record_channel = CONTAINEROF(plug, RecordChannel, plug);

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

static void snd_record_stop(RecordPlug *plug)
{
    RecordChannel *record_channel = CONTAINEROF(plug, RecordChannel, plug);

    ASSERT(record_channel->base.active);
    record_channel->base.active = FALSE;
    if (record_channel->base.client_active) {
        snd_set_command(&record_channel->base, SND_RECORD_CTRL_MASK);
        snd_record_send(&record_channel->base);
    } else {
        record_channel->base.command &= ~SND_RECORD_CTRL_MASK;
    }
}

static uint32_t snd_record_read(RecordPlug *plug, uint32_t num_samples, uint32_t *samples)
{
    RecordChannel *record_channel = CONTAINEROF(plug, RecordChannel, plug);
    uint32_t read_pos;
    uint32_t now;
    uint32_t len;

    ASSERT(record_channel->base.active);

    if (record_channel->write_pos < RECORD_SAMPLES_SIZE / 2) {
        return 0;
    }

    len = MIN(record_channel->write_pos - record_channel->read_pos, num_samples);

    if (len < num_samples) {
        SndWorker *worker = record_channel->base.worker;
        snd_receive(record_channel);
        if (!worker->connection) {
            return 0;
        }
        len = MIN(record_channel->write_pos - record_channel->read_pos, num_samples);
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
    RecordInterface *interface = (RecordInterface *)worker->interface;
    ASSERT(record_channel);

    record_channel->plug_ref = interface->plug(interface, &record_channel->plug,
                                               &record_channel->base.active);
    if (!record_channel->base.migrate) {
        if (record_channel->base.active) {
            snd_set_command((SndChannel *)record_channel, SND_RECORD_CTRL_MASK);
        }
    }
}

static void snd_record_cleanup(SndChannel *channel)
{
    RecordChannel *record_channel = (RecordChannel *)channel;
    RecordInterface *interface = (RecordInterface *)channel->worker->interface;
    interface->unplug(interface, record_channel->plug_ref);

    celt051_decoder_destroy(record_channel->celt_decoder);
    celt051_mode_destroy(record_channel->celt_mode);
}

static void snd_set_record_peer(Channel *channel, RedsStreamContext *peer, int migration,
                                int num_common_caps, uint32_t *common_caps, int num_caps,
                                uint32_t *caps)
{
    SndWorker *worker = (SndWorker *)channel;
    RecordChannel *record_channel;
    CELTDecoder *celt_decoder;
    CELTMode *celt_mode;
    int celt_error;

    snd_disconnect_channel(worker->connection);

    if (!(celt_mode = celt051_mode_create(VD_INTERFACE_RECORD_FREQ, VD_INTERFACE_RECORD_CHAN,
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
                                                          peer,
                                                          migration,
                                                          snd_record_send,
                                                          snd_record_handle_message,
                                                          snd_record_on_message_done,
                                                          snd_record_cleanup))) {
        goto error_2;
    }

    worker->connection = &record_channel->base;

    record_channel->plug.major_version = VD_INTERFACE_RECORD_MAJOR;
    record_channel->plug.minor_version = VD_INTERFACE_RECORD_MINOR;
    record_channel->plug.start = snd_record_start;
    record_channel->plug.stop = snd_record_stop;
    record_channel->plug.read = snd_record_read;
    record_channel->celt_mode = celt_mode;
    record_channel->celt_decoder = celt_decoder;

    on_new_record_channel(worker);
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

static SndWorker *find_worker(VDInterface *interface)
{
    SndWorker *worker = workers;
    while (worker) {
        if (worker->interface == interface) {
            break;
        }
        worker = worker->next;
    }
    return worker;
}

void snd_attach_playback(PlaybackInterface *interface)
{
    SndWorker *playback_worker;
    if (!(playback_worker = (SndWorker *)malloc(sizeof(*playback_worker)))) {
        red_error("playback channel malloc failed");
    }
    memset(playback_worker, 0, sizeof(*playback_worker));
    playback_worker->base.type = RED_CHANNEL_PLAYBACK;
    playback_worker->base.link = snd_set_playback_peer;
    playback_worker->base.shutdown = snd_shutdown;
    playback_worker->base.migrate = snd_playback_migrate;
    playback_worker->base.data = NULL;

    playback_worker->interface = &interface->base;
    playback_worker->base.num_caps = 1;
    if (!(playback_worker->base.caps = malloc(sizeof(uint32_t)))) {
        PANIC("malloc failed");
    }
    playback_worker->base.caps[0] = (1 << RED_PLAYBACK_CAP_CELT_0_5_1);

    add_worker(playback_worker);
    reds_register_channel(&playback_worker->base);
}

void snd_attach_record(RecordInterface *interface)
{
    SndWorker *record_worker;
    if (!(record_worker = (SndWorker *)malloc(sizeof(*record_worker)))) {
        PANIC("malloc failed");
    }

    memset(record_worker, 0, sizeof(*record_worker));
    record_worker->base.type = RED_CHANNEL_RECORD;
    record_worker->base.link = snd_set_record_peer;
    record_worker->base.shutdown = snd_shutdown;
    record_worker->base.migrate = snd_record_migrate;
    record_worker->base.data = NULL;

    record_worker->interface = &interface->base;

    record_worker->base.num_caps = 1;
    if (!(record_worker->base.caps = malloc(sizeof(uint32_t)))) {
        PANIC("malloc failed");
    }
    record_worker->base.caps[0] = (1 << RED_RECORD_CAP_CELT_0_5_1);
    add_worker(record_worker);
    reds_register_channel(&record_worker->base);
}

static void snd_detach_common(VDInterface *interface)
{
    SndWorker *worker = find_worker(interface);

    if (!worker) {
        return;
    }
    remove_worker(worker);
    snd_disconnect_channel(worker->connection);
    reds_unregister_channel(&worker->base);

    free(worker->base.common_caps);
    free(worker->base.caps);
    free(worker);
}

void snd_detach_playback(PlaybackInterface *interface)
{
    snd_detach_common(&interface->base);
}

void snd_detach_record(RecordInterface *interface)
{
    snd_detach_common(&interface->base);
}

void snd_set_playback_compression(int on)
{
    SndWorker *now = workers;

    playback_compression = on ? RED_AUDIO_DATA_MODE_CELT_0_5_1 : RED_AUDIO_DATA_MODE_RAW;
    for (; now; now = now->next) {
        if (now->base.type == RED_CHANNEL_PLAYBACK && now->connection) {
            PlaybackChannel* playback = (PlaybackChannel*)now->connection;
            if (!playback->celt_allowed) {
                ASSERT(playback->mode == RED_AUDIO_DATA_MODE_RAW);
                continue;
            }
            if (playback->mode != playback_compression) {
                playback->mode = playback_compression;
                snd_set_command(now->connection, SND_PLAYBACK_MODE_MASK);
            }
        }
    }
}

int snd_get_playback_compression()
{
    return (playback_compression == RED_AUDIO_DATA_MODE_RAW) ? FALSE : TRUE;
}

