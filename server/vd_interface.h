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

#ifndef _H_VD_INTERFACE
#define _H_VD_INTERFACE

#include <stdint.h>

#define VM_INTERFACE_VERSION 1
typedef unsigned long VDObjectRef;
#define INVALID_VD_OBJECT_REF 0
typedef struct VDInterface VDInterface;

struct VDInterface {
    uint32_t base_version;
    const char *type;
    unsigned int id;
    const char *description;
    //todo: swap minor major order on VM_INTERFACE_VERSION change
    //      (here and in spacific interfaces)
    uint32_t minor_version;
    uint32_t major_version;
};

#define VD_INTERFACE_CORE "core"
#define VD_INTERFACE_CORE_MAJOR 1
#define VD_INTERFACE_CORE_MINOR 2
typedef struct CoreInterface CoreInterface;
typedef enum {
    VD_INTERFACE_ADDING,
    VD_INTERFACE_REMOVING,
} VDInterfaceChangeType;

typedef enum {
    VD_LOG_ERROR = 1,
    VD_LOG_WARN,
    VD_LOG_INFO,
} LogLevel;

typedef void (*vd_interface_change_notifier_t)(void *opaque, VDInterface *interface,
                                               VDInterfaceChangeType change);
typedef void (*timer_callback_t)(void *opaque);

#define SPICE_WATCH_EVENT_READ  (1 << 0)
#define SPICE_WATCH_EVENT_WRITE (1 << 1)

typedef struct SpiceWatch SpiceWatch;
typedef void (*SpiceWatchFunc)(int fd, int event, void *opaque);

struct CoreInterface {
    VDInterface base;

    VDObjectRef (*create_timer)(CoreInterface *core, timer_callback_t, void *opaue);
    void (*arm_timer)(CoreInterface *core, VDObjectRef timer, uint32_t ms);
    void (*disarm_timer)(CoreInterface *core, VDObjectRef timer);
    void (*destroy_timer)(CoreInterface *core, VDObjectRef timer);

    SpiceWatch *(*watch_add)(int fd, int event_mask, SpiceWatchFunc func, void *opaque);
    void (*watch_update_mask)(SpiceWatch *watch, int event_mask);
    void (*watch_remove)(SpiceWatch *watch);

};

#define VD_INTERFACE_QXL "qxl"
#define VD_INTERFACE_QXL_MAJOR 3
#define VD_INTERFACE_QXL_MINOR 0
typedef struct QXLInterface QXLInterface;
typedef void (*qxl_mode_change_notifier_t)(void *opaque);
typedef struct QXLWorker QXLWorker;
typedef struct QXLDevMemSlot QXLDevMemSlot;
typedef struct QXLDevSurfaceCreate QXLDevSurfaceCreate;
union QXLReleaseInfo;
struct QXLReleaseInfoExt;
struct QXLCommand;
struct QXLCommandExt;
struct SpiceRect;
struct QXLWorker {
    uint32_t minor_version;
    uint32_t major_version;
    void (*wakeup)(QXLWorker *worker);
    void (*oom)(QXLWorker *worker);
    void (*save)(QXLWorker *worker);
    void (*load)(QXLWorker *worker);
    void (*start)(QXLWorker *worker);
    void (*stop)(QXLWorker *worker);
    void (*update_area)(QXLWorker *qxl_worker, uint32_t surface_id,
                       struct SpiceRect *area, struct SpiceRect *dirty_rects,
                       uint32_t num_dirty_rects, uint32_t clear_dirty_region);
    void (*add_memslot)(QXLWorker *worker, QXLDevMemSlot *slot);
    void (*del_memslot)(QXLWorker *worker, uint32_t slot_group_id, uint32_t slot_id);
    void (*reset_memslots)(QXLWorker *worker);
    void (*destroy_surfaces)(QXLWorker *worker);
    void (*destroy_primary_surface)(QXLWorker *worker, uint32_t surface_id);
    void (*create_primary_surface)(QXLWorker *worker, uint32_t surface_id,
                                   QXLDevSurfaceCreate *surface);
    void (*reset_image_cache)(QXLWorker *worker);
    void (*reset_cursor)(QXLWorker *worker);
    void (*destroy_surface_wait)(QXLWorker *worker, uint32_t surface_id);
};

typedef struct DrawArea {
    uint8_t *buf;
    uint32_t size;
    uint8_t *line_0;
    uint32_t width;
    uint32_t heigth;
    int stride;
} DrawArea;

typedef struct QXLDevInfo {
    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
    uint32_t use_hardware_cursor;

    DrawArea draw_area;

    uint32_t ram_size;
} QXLDevInfo;

typedef struct QXLDevInitInfo {
    uint32_t num_memslots_groups;
    uint32_t num_memslots;
    uint8_t memslot_gen_bits;
    uint8_t memslot_id_bits;
    uint32_t qxl_ram_size;
    uint8_t internal_groupslot_id;
    uint32_t n_surfaces;
} QXLDevInitInfo;

struct QXLDevMemSlot {
    uint32_t slot_group_id;
    uint32_t slot_id;
    uint32_t generation;
    unsigned long virt_start;
    unsigned long virt_end;
    uint64_t addr_delta;
    uint32_t qxl_ram_size;
};

struct QXLDevSurfaceCreate {
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t format;
    uint32_t position;
    uint32_t mouse_mode;
    uint32_t flags;
    uint32_t type;
    uint64_t mem;
    uint32_t group_id;
};

struct SpiceRect;

struct QXLInterface {
    VDInterface base;

    uint16_t pci_vendor;
    uint16_t pci_id;
    uint8_t pci_revision;

    void (*attache_worker)(QXLInterface *qxl, QXLWorker *qxl_worker);
    void (*set_compression_level)(QXLInterface *qxl, int level);
    void (*set_mm_time)(QXLInterface *qxl, uint32_t mm_time);

    void (*get_init_info)(QXLInterface *qxl, QXLDevInitInfo *info);
    int (*get_command)(QXLInterface *qxl, struct QXLCommandExt *cmd);
    int (*req_cmd_notification)(QXLInterface *qxl);
    int (*has_command)(QXLInterface *qxl);
    void (*release_resource)(QXLInterface *qxl, struct QXLReleaseInfoExt release_info);
    int (*get_cursor_command)(QXLInterface *qxl, struct QXLCommandExt *cmd);
    int (*req_cursor_notification)(QXLInterface *qxl);
    void (*notify_update)(QXLInterface *qxl, uint32_t update_id);
    void (*set_save_data)(QXLInterface *qxl, void *data, int size);
    void *(*get_save_data)(QXLInterface *qxl);
    int (*flush_resources)(QXLInterface *qxl);
};

#define VD_INTERFACE_KEYBOARD "keyboard"
#define VD_INTERFACE_KEYBOARD_MAJOR 1
#define VD_INTERFACE_KEYBOARD_MINOR 1
typedef struct KeyboardInterface KeyboardInterface;
typedef void (*keyborad_leads_notifier_t)(void *opaque, uint8_t leds);

struct KeyboardInterface {
    VDInterface base;

    void (*push_scan_freg)(KeyboardInterface *keyboard, uint8_t frag);
    uint8_t (*get_leds)(KeyboardInterface *keyboard);
    VDObjectRef (*register_leds_notifier)(KeyboardInterface *keyboard,
                                          keyborad_leads_notifier_t notifier, void *opaque);
    void (*unregister_leds_notifayer)(KeyboardInterface *keyboard, VDObjectRef notifier);
};

#define VD_INTERFACE_MOUSE "mouse"
#define VD_INTERFACE_MOUSE_MAJOR 1
#define VD_INTERFACE_MOUSE_MINOR 1
typedef struct MouseInterface MouseInterface;

struct MouseInterface {
    VDInterface base;

    void (*moution)(MouseInterface* mouse, int dx, int dy, int dz,
                    uint32_t buttons_state);
    void (*buttons)(MouseInterface* mouse, uint32_t buttons_state);
};

#define VD_INTERFACE_TABLET "tablet"
#define VD_INTERFACE_TABLET_MAJOR 1
#define VD_INTERFACE_TABLET_MINOR 1
typedef struct TabletInterface TabletInterface;

struct TabletInterface {
    VDInterface base;

    void (*set_logical_size)(TabletInterface* tablet, int width, int height);
    void (*position)(TabletInterface* tablet, int x, int y, uint32_t buttons_state);
    void (*wheel)(TabletInterface* tablet, int wheel_moution, uint32_t buttons_state);
    void (*buttons)(TabletInterface* tablet, uint32_t buttons_state);
};

#define VD_INTERFACE_MIGRATION "migration"
#define VD_INTERFACE_MIGRATION_MAJOR 1
#define VD_INTERFACE_MIGRATION_MINOR 1
typedef struct MigrationInterface MigrationInterface;
typedef void (*migration_notify_started_t)(void *opaque, const char *args);
typedef void (*migration_notify_finished_t)(void *opaque, int completed);
typedef void (*migration_notify_recv_t)(void *opaque, int fd);

struct MigrationInterface {
    VDInterface base;

    VDObjectRef (*register_notifiers)(MigrationInterface* mig, const char *key,
                                      migration_notify_started_t,
                                      migration_notify_finished_t,
                                      migration_notify_recv_t,
                                      void *opaque);
    void (*unregister_notifiers)(MigrationInterface* mig, VDObjectRef notifier);
    void (*notifier_done)(MigrationInterface *mig, VDObjectRef notifier);
    int (*begin_hook)(MigrationInterface *mig, VDObjectRef notifier);
};

enum VDIArgType{
    ARG_TYPE_INVALID,
    ARG_TYPE_INT,
    ARG_TYPE_STRING,
};

typedef struct VDIArgDescriptor {
    char* name;
    int type;
    int optional;
} VDIArgDescriptor;

typedef struct VDICmdArg {
    VDIArgDescriptor descriptor;
    union {
        uint64_t int_val;
        const char *string_val;
    };
} VDICmdArg;

typedef void (*VDICmdHandler)(const VDICmdArg* args);
typedef void (*VDIInfoCmdHandler)(void);

#define VD_INTERFACE_PLAYBACK "playback"
#define VD_INTERFACE_PLAYBACK_MAJOR 1
#define VD_INTERFACE_PLAYBACK_MINOR 1
typedef struct PlaybackInterface PlaybackInterface;

enum {
    VD_INTERFACE_AUDIO_FMT_S16 = 1,
};

#define VD_INTERFACE_PLAYBACK_FREQ 44100
#define VD_INTERFACE_PLAYBACK_CHAN 2
#define VD_INTERFACE_PLAYBACK_FMT VD_INTERFACE_AUDIO_FMT_S16

typedef struct PlaybackPlug PlaybackPlug;
struct PlaybackPlug {
    uint32_t minor_version;
    uint32_t major_version;
    void (*start)(PlaybackPlug *plug);
    void (*stop)(PlaybackPlug *plug);
    void (*get_frame)(PlaybackPlug *plug, uint32_t **frame, uint32_t *samples);
    void (*put_frame)(PlaybackPlug *plug, uint32_t *frame);
};

struct PlaybackInterface {
    VDInterface base;

    VDObjectRef (*plug)(PlaybackInterface *playback, PlaybackPlug* plug, int *enable);
    void (*unplug)(PlaybackInterface *playback, VDObjectRef);
};

#define VD_INTERFACE_RECORD "record"
#define VD_INTERFACE_RECORD_MAJOR 2
#define VD_INTERFACE_RECORD_MINOR 1
typedef struct RecordInterface RecordInterface;

#define VD_INTERFACE_RECORD_FREQ 44100
#define VD_INTERFACE_RECORD_CHAN 2
#define VD_INTERFACE_RECORD_FMT VD_INTERFACE_AUDIO_FMT_S16


typedef struct RecordPlug RecordPlug;
struct RecordPlug {
    uint32_t minor_version;
    uint32_t major_version;
    void (*start)(RecordPlug *plug);
    void (*stop)(RecordPlug *plug);
    uint32_t (*read)(RecordPlug *plug, uint32_t num_samples, uint32_t *samples);
};

struct RecordInterface {
    VDInterface base;

    VDObjectRef (*plug)(RecordInterface *recorder, RecordPlug* plug, int *enable);
    void (*unplug)(RecordInterface *recorder, VDObjectRef);
};

#define VD_INTERFACE_VDI_PORT "vdi_port"
#define VD_INTERFACE_VDI_PORT_MAJOR 1
#define VD_INTERFACE_VDI_PORT_MINOR 1
typedef struct VDIPortInterface VDIPortInterface;

typedef struct VDIPortPlug VDIPortPlug;
struct VDIPortPlug {
    uint32_t minor_version;
    uint32_t major_version;
    void (*wakeup)(VDIPortPlug *plug);
};

struct VDIPortInterface {
    VDInterface base;

    VDObjectRef (*plug)(VDIPortInterface *port, VDIPortPlug* plug);
    void (*unplug)(VDIPortInterface *port, VDObjectRef plug);
    int (*write)(VDIPortInterface *port, VDObjectRef plug, const uint8_t *buf, int len);
    int (*read)(VDIPortInterface *port, VDObjectRef plug, uint8_t *buf, int len);
};

#define VD_INTERFACE_NET_WIRE "net_wire"
#define VD_INTERFACE_NET_WIRE_MAJOR 1
#define VD_INTERFACE_NET_WIRE_MINOR 1

typedef struct NetWireInterface NetWireInterface;
typedef void (*net_wire_packet_route_proc_t)(void *opaque, const uint8_t *pkt, int pkt_len);

struct NetWireInterface {
    VDInterface base;

    struct in_addr (*get_ip)(NetWireInterface *vlan);
    int (*can_send_packet)(NetWireInterface *vlan);
    void (*send_packet)(NetWireInterface *vlan, const uint8_t *buf, int size);
    VDObjectRef (*register_route_packet)(NetWireInterface *vlan, net_wire_packet_route_proc_t proc,
                                         void *opaque);
    void (*unregister_route_packet)(NetWireInterface *vlan, VDObjectRef proc);
};

#endif

