#pragma once

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <stdbool.h>

struct HIDGamepad {
	int padIndex;
	IOHIDDeviceRef hidDeviceRef;
	IOHIDQueueRef hidQueueRef;
	int hidDeviceVendorID;
	int hidDeviceProductID;
	char hidDeviceVendor[64];
	char hidDeviceProduct[64];

	IOHIDElementCookie axis[6];
	IOHIDElementCookie buttons[15];
	// Which axis / buttons slots hold a cookie: 0 is not reserved, so an unset slot cannot be told apart by its value.
	bool axisSet[6];
	bool buttonsSet[15];
};

void HIDGamepad_init(struct HIDGamepad *gamepad);
void HIDGamepad_destroy(struct HIDGamepad *gamepad);
void HIDGamepad_bind(struct HIDGamepad *gamepad, IOHIDDeviceRef deviceRef, int padIndex);
void HIDGamepad_unbind(struct HIDGamepad *gamepad);

// Defined in system.c.h; records the ids kinc_gamepad_vendor_id/_product_id report (-1, -1 forgets them).
void kinc_macos_internal_gamepad_set_hid_ids(int gamepad, int vendor_id, int product_id);
