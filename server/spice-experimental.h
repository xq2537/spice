/* vdi port interface */

#define SPICE_INTERFACE_VDI_PORT "vdi_port"
#define SPICE_INTERFACE_VDI_PORT_MAJOR 1
#define SPICE_INTERFACE_VDI_PORT_MINOR 1
typedef struct SpiceVDIPortInterface SpiceVDIPortInterface;
typedef struct SpiceVDIPortInstance SpiceVDIPortInstance;
typedef struct SpiceVDIPortState SpiceVDIPortState;

struct SpiceVDIPortInterface {
    SpiceBaseInterface base;

    void (*state)(SpiceVDIPortInstance *sin, int connected);
    int (*write)(SpiceVDIPortInstance *sin, const uint8_t *buf, int len);
    int (*read)(SpiceVDIPortInstance *sin, uint8_t *buf, int len);
};

struct SpiceVDIPortInstance {
    SpiceBaseInstance base;
    SpiceVDIPortState *st;
};

void spice_server_vdi_port_wakeup(SpiceVDIPortInstance *sin);

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

