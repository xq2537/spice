#ifndef __CHAR_DEVICE_H__
#define __CHAR_DEVICE_H__

#include "spice.h"

struct SpiceCharDeviceState {
    void (*wakeup)(SpiceCharDeviceInstance *sin);
};

int usbredir_device_connect(SpiceCharDeviceInstance *char_device);
void usbredir_device_disconnect(SpiceCharDeviceInstance *char_device);

#endif // __CHAR_DEVICE_H__

