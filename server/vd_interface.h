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

struct CoreInterface {
    VDInterface base;

    VDInterface *(*next)(CoreInterface *core, VDInterface *prev);

    VDObjectRef (*register_change_notifiers)(CoreInterface *core, void *opaque,
                                             vd_interface_change_notifier_t in_notifier);
    void (*unregister_change_notifiers)(CoreInterface *core, VDObjectRef notifier);

    VDObjectRef (*create_timer)(CoreInterface *core, timer_callback_t, void *opaue);
    void (*arm_timer)(CoreInterface *core, VDObjectRef timer, uint32_t ms);
    void (*disarm_timer)(CoreInterface *core, VDObjectRef timer);
    void (*destroy_timer)(CoreInterface *core, VDObjectRef timer);

    int (*set_file_handlers)(CoreInterface *core, int fd,
                             void (*on_read)(void *),
                             void (*on_write)(void *),
                             void *opaque);

    void (*term_printf)(CoreInterface *core, const char* str, ...);
    void (*log)(CoreInterface *core, LogLevel level, const char* component,
                const char* format, ...);
};

#define VD_INTERFACE_QXL "qxl"
#define VD_INTERFACE_QXL_MAJOR 1
#define VD_INTERFACE_QXL_MINOR 2
typedef struct QXLInterface QXLInterface;
typedef void (*qxl_mode_change_notifier_t)(void *opaque);
typedef struct QXLWorker QXLWorker;
union QXLReleaseInfo;
struct QXLCommand;
struct QXLWorker {
    uint32_t minor_version;
    uint32_t major_version;
    void (*attach)(QXLWorker *worker);
    void (*detach)(QXLWorker *worker);
    void (*wakeup)(QXLWorker *worker);
    void (*oom)(QXLWorker *worker);
    void (*save)(QXLWorker *worker);
    void (*load)(QXLWorker *worker);
    void (*start)(QXLWorker *worker);
    void (*stop)(QXLWorker *worker);
    void (*update_area)(QXLWorker *worker);
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
    long phys_delta;
    unsigned long phys_start;
    unsigned long phys_end;

    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
    uint32_t use_hardware_cursor;

    DrawArea draw_area;

    uint32_t ram_size;
} QXLDevInfo;

struct QXLInterface {
    VDInterface base;

    uint16_t pci_vendor;
    uint16_t pci_id;
    uint8_t pci_revision;

    void (*attache_worker)(QXLInterface *qxl, QXLWorker *qxl_worker);
    void (*set_compression_level)(QXLInterface *qxl, int level);
    void (*set_mm_time)(QXLInterface *qxl, uint32_t mm_time);
    VDObjectRef (*register_mode_change)(QXLInterface *qxl, qxl_mode_change_notifier_t,
                                        void *opaque);
    void (*unregister_mode_change)(QXLInterface *qxl, VDObjectRef notifier);

    void (*get_info)(QXLInterface *qxl, QXLDevInfo *info);
    int (*get_command)(QXLInterface *qxl, struct QXLCommand *cmd);
    int (*req_cmd_notification)(QXLInterface *qxl);
    int (*has_command)(QXLInterface *qxl);
    void (*release_resource)(QXLInterface *qxl, union QXLReleaseInfo *release_info);
    int (*get_cursor_command)(QXLInterface *qxl, struct QXLCommand *cmd);
    int (*req_cursor_notification)(QXLInterface *qxl);
    const struct Rect *(*get_update_area)(QXLInterface *qxl);
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

#define VD_INTERFACE_QTERM "qemu_terminal"
#define VD_INTERFACE_QTERM_MAJOR 1
#define VD_INTERFACE_QTERM_MINOR 1
typedef struct QTermInterface QTermInterface;

struct QTermInterface {
    VDInterface base;

    VDObjectRef (*add_action_command_handler)(QTermInterface *term, const char *module_name,
                                              const char *name,
                                              const char *args_type,
                                              void *handler,
                                              const char *params,
                                              const char *help);
    void (*remove_action_command_handler)(QTermInterface *term, VDObjectRef obj);

    VDObjectRef (*add_info_command_handler)(QTermInterface *term, const char *module_name,
                                            const char *name,
                                            void *handler,
                                            const char *help);
    void (*remove_info_command_handler)(QTermInterface *term, VDObjectRef obj);
};

#define VD_INTERFACE_QTERM2 "qemu_terminal_2"
#define VD_INTERFACE_QTERM2_MAJOR 1
#define VD_INTERFACE_QTERM2_MINOR 0
typedef struct QTerm2Interface QTerm2Interface;

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

struct QTerm2Interface {
    VDInterface base;

    VDObjectRef (*add_action_command_handler)(QTerm2Interface *term,
                                              const char *module_name,
                                              const char *command_name,
                                              const VDIArgDescriptor *args_type,
                                              VDICmdHandler handler,
                                              const char *params_text,
                                              const char *help_text);

    void (*remove_action_command_handler)(QTerm2Interface *term, VDObjectRef obj);

    VDObjectRef (*add_info_command_handler)(QTerm2Interface *term,
                                            const char *module_name,
                                            const char *command_name,
                                            VDIInfoCmdHandler handler,
                                            const char *help_text);

    void (*remove_info_command_handler)(QTerm2Interface *term, VDObjectRef obj);
};

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

#endif

