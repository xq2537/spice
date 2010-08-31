/* char device interfaces */

#define SPICE_INTERFACE_CHAR_DEVICE "char_device"
#define SPICE_INTERFACE_CHAR_DEVICE_MAJOR 1
#define SPICE_INTERFACE_CHAR_DEVICE_MINOR 1
typedef struct SpiceCharDeviceInterface SpiceCharDeviceInterface;
typedef struct SpiceCharDeviceInstance SpiceCharDeviceInstance;
typedef struct SpiceCharDeviceState SpiceCharDeviceState;

struct SpiceCharDeviceInterface {
    SpiceBaseInterface base;

    void (*state)(SpiceCharDeviceInstance *sin, int connected);
    int (*write)(SpiceCharDeviceInstance *sin, const uint8_t *buf, int len);
    int (*read)(SpiceCharDeviceInstance *sin, uint8_t *buf, int len);
};

struct SpiceCharDeviceInstance {
    SpiceBaseInstance base;
    SpiceCharDeviceState *st;
};

void spice_server_char_device_wakeup(SpiceCharDeviceInstance *sin);

/* tunnel interface */

#define SPICE_INTERFACE_NET_WIRE "net_wire"
#define SPICE_INTERFACE_NET_WIRE_MAJOR 1
#define SPICE_INTERFACE_NET_WIRE_MINOR 1
typedef struct SpiceNetWireInterface SpiceNetWireInterface;
typedef struct SpiceNetWireInstance SpiceNetWireInstance;
typedef struct SpiceNetWireState SpiceNetWireState;

struct SpiceNetWireInterface {
    SpiceBaseInterface base;

    struct in_addr (*get_ip)(SpiceNetWireInstance *sin);
    int (*can_send_packet)(SpiceNetWireInstance *sin);
    void (*send_packet)(SpiceNetWireInstance *sin, const uint8_t *pkt, int len);
};

struct SpiceNetWireInstance {
    SpiceBaseInstance base;
    SpiceNetWireState *st;
};

void spice_server_net_wire_recv_packet(SpiceNetWireInstance *sin,
                                       const uint8_t *pkt, int len);

/* spice client migration */

enum {
    SPICE_MIGRATE_CLIENT_NONE = 1,
    SPICE_MIGRATE_CLIENT_WAITING,
    SPICE_MIGRATE_CLIENT_READY,
};

int spice_server_migrate_info(SpiceServer *s, const char* dest, int port, int secure_port,
                              const char* cert_subject);
int spice_server_migrate_start(SpiceServer *s);
int spice_server_migrate_client_state(SpiceServer *s);
int spice_server_migrate_end(SpiceServer *s, int completed);

