#ifndef __CHAR_DEVICE_H__
#define __CHAR_DEVICE_H__

#include "spice.h"

struct SpiceCharDeviceState {
    void (*wakeup)(SpiceCharDeviceInstance *sin);
};

void spicevmc_device_connect(SpiceCharDeviceInstance *sin,
                             uint8_t channel_type);
void spicevmc_device_disconnect(SpiceCharDeviceInstance *char_device);

#endif // __CHAR_DEVICE_H__
