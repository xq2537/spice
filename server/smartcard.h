#ifndef __SMART_CARD_H__
#define __SMART_CARD_H__

#include "server/spice-experimental.h"

// Maximal length of APDU
#define APDUBufSize 270

/** connect to smartcard interface, used by smartcard channel
 * returns -1 if failed, 0 if successfull
 */
int smartcard_device_connect(SpiceCharDeviceInstance *char_device);
void smartcard_device_disconnect(SpiceCharDeviceInstance *char_device);

void smartcard_channel_init();

#endif // __SMART_CARD_H__

