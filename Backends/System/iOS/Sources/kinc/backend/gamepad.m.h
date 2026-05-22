// iOS gamepad bridge — implements kinc_gamepad_* via the
// GameController.framework (MFi controllers).  Compiled as part of
// iosunit.m.  Mirrors the button/axis indexing conventions used by the
// Android backend so cross-platform games see the same layout.
//
// Button index map (matches Android system.c.h:374-416):
//   0 A           4 L1            8  Select/Options    12 DPad Up
//   1 B           5 R1            9  Start/Menu        13 DPad Down
//   2 X           6 L2 (analog)   10 Left Thumb        14 DPad Left
//   3 Y           7 R2 (analog)   11 Right Thumb       15 DPad Right
//
// Axis index map:
//   0 Left X    1 Left Y (Y-down to match Android)
//   2 Right X   3 Right Y (Y-down to match Android)
//
// Disable the whole compilation unit with KINC_NO_GAMEPAD_IOS to drop
// the GameController.framework linkage entirely (App Store submission
// concern for games that don't declare gamepad use).

#ifndef KINC_NO_GAMEPAD_IOS

#import <GameController/GameController.h>

#include <kinc/input/gamepad.h>
#include <kinc/log.h>
#include <stddef.h>
#include <stdio.h>

// Tracked controllers, indexed by Kha gamepad slot (0..3).  We never
// hand out more than 4 slots; extras get ignored.
#define KINC_IOS_GAMEPAD_MAX 4
static GCController *kinc_ios_gamepads[KINC_IOS_GAMEPAD_MAX] = {nil, nil, nil, nil};
static char kinc_ios_gamepad_vendor_buf[KINC_IOS_GAMEPAD_MAX][128];
static char kinc_ios_gamepad_product_buf[KINC_IOS_GAMEPAD_MAX][128];

static int kinc_ios_gamepad_slot(GCController *controller) {
	for (int i = 0; i < KINC_IOS_GAMEPAD_MAX; ++i) {
		if (kinc_ios_gamepads[i] == controller) return i;
	}
	return -1;
}

static int kinc_ios_gamepad_assign_slot(GCController *controller) {
	for (int i = 0; i < KINC_IOS_GAMEPAD_MAX; ++i) {
		if (kinc_ios_gamepads[i] == nil) {
			kinc_ios_gamepads[i] = controller;
			return i;
		}
	}
	return -1;
}

static void kinc_ios_gamepad_emit_button(int slot, int index, BOOL pressed, float value) {
	(void)pressed;  // Kha uses the analog value (0.0 / 1.0 for digital, 0..1 for triggers)
	kinc_internal_gamepad_trigger_button(slot, index, value);
}

static void kinc_ios_gamepad_attach_handlers(GCController *controller, int slot) {
	GCExtendedGamepad *pad = controller.extendedGamepad;
	if (pad == nil) return;  // micro / non-extended profiles aren't supported

	// Face buttons
	pad.buttonA.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 0, pressed, value);
	};
	pad.buttonB.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 1, pressed, value);
	};
	pad.buttonX.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 2, pressed, value);
	};
	pad.buttonY.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 3, pressed, value);
	};

	// Shoulder buttons + analog triggers
	pad.leftShoulder.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 4, pressed, value);
	};
	pad.rightShoulder.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 5, pressed, value);
	};
	pad.leftTrigger.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 6, pressed, value);
	};
	pad.rightTrigger.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 7, pressed, value);
	};

	// Menu / options (Select/Start in Android terms).  Both properties
	// are iOS 13+; on older systems we silently skip these buttons.
	if (@available(iOS 13.0, *)) {
		if (pad.buttonOptions != nil) {
			pad.buttonOptions.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
				kinc_ios_gamepad_emit_button(slot, 8, pressed, value);
			};
		}
		pad.buttonMenu.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
			kinc_ios_gamepad_emit_button(slot, 9, pressed, value);
		};
	}

	// Thumbstick clicks (iOS 12.1+)
	if (@available(iOS 12.1, *)) {
		if (pad.leftThumbstickButton != nil) {
			pad.leftThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
				kinc_ios_gamepad_emit_button(slot, 10, pressed, value);
			};
		}
		if (pad.rightThumbstickButton != nil) {
			pad.rightThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
				kinc_ios_gamepad_emit_button(slot, 11, pressed, value);
			};
		}
	}

	// DPad — emit edge-triggered button events for up/down/left/right
	pad.dpad.up.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 12, pressed, value);
	};
	pad.dpad.down.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 13, pressed, value);
	};
	pad.dpad.left.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 14, pressed, value);
	};
	pad.dpad.right.valueChangedHandler = ^(GCControllerButtonInput * _Nonnull btn, float value, BOOL pressed) {
		kinc_ios_gamepad_emit_button(slot, 15, pressed, value);
	};

	// Sticks — Y is negated to match Android convention (Y-down).
	pad.leftThumbstick.xAxis.valueChangedHandler = ^(GCControllerAxisInput * _Nonnull axis, float value) {
		kinc_internal_gamepad_trigger_axis(slot, 0, value);
	};
	pad.leftThumbstick.yAxis.valueChangedHandler = ^(GCControllerAxisInput * _Nonnull axis, float value) {
		kinc_internal_gamepad_trigger_axis(slot, 1, -value);
	};
	pad.rightThumbstick.xAxis.valueChangedHandler = ^(GCControllerAxisInput * _Nonnull axis, float value) {
		kinc_internal_gamepad_trigger_axis(slot, 2, value);
	};
	pad.rightThumbstick.yAxis.valueChangedHandler = ^(GCControllerAxisInput * _Nonnull axis, float value) {
		kinc_internal_gamepad_trigger_axis(slot, 3, -value);
	};
}

static void kinc_ios_gamepad_register(GCController *controller) {
	if (kinc_ios_gamepad_slot(controller) >= 0) return;  // already known

	// Skip controllers that aren't real input devices.  Phantom sources:
	//  1. extendedGamepad nil — micro / Apple TV remote / non-extended
	//     profiles that can't drive game input.
	//  2. iOS Simulator virtual controller — fires
	//     GCControllerDidConnectNotification with a synthetic GCController
	//     whose productCategory == "MFi" and vendorName == "Gamepad".
	//     Real MFi accessories report a brand-specific vendorName
	//     (e.g. "Sony Computer Entertainment Wireless Controller",
	//     "Backbone One", "Razer Kishi"), never the literal "Gamepad".
	//  3. Empty / nil vendorName — defensive: real controllers always
	//     populate it.
	if (controller.extendedGamepad == nil) {
		kinc_log(KINC_LOG_LEVEL_INFO, "[gamepad] skip: no extendedGamepad");
		return;
	}
	NSString *vendor = controller.vendorName;
	NSString *category = @"";
	if (@available(iOS 13.0, *)) {
		category = controller.productCategory != nil ? controller.productCategory : @"";
	}
	if (vendor == nil || vendor.length == 0) {
		kinc_log(KINC_LOG_LEVEL_INFO, "[gamepad] skip: empty vendorName");
		return;
	}
	if ([vendor isEqualToString:@"Gamepad"] && [category isEqualToString:@"MFi"]) {
		kinc_log(KINC_LOG_LEVEL_INFO, "[gamepad] skip: simulator phantom (MFi/Gamepad)");
		return;
	}

	int slot = kinc_ios_gamepad_assign_slot(controller);
	if (slot < 0) return;  // out of slots
	kinc_ios_gamepad_attach_handlers(controller, slot);
	kinc_internal_gamepad_trigger_connect(slot);
}

static void kinc_ios_gamepad_unregister(GCController *controller) {
	int slot = kinc_ios_gamepad_slot(controller);
	if (slot < 0) return;
	kinc_ios_gamepads[slot] = nil;
	kinc_internal_gamepad_trigger_disconnect(slot);
}

// One-shot setup.  Called from kinc_internal_init_gamepad (or via a
// guard on first kinc_gamepad_* call) so we don't attach observers
// before the GameController framework is reachable.
static BOOL kinc_ios_gamepad_inited = NO;

static void kinc_ios_gamepad_init(void) {
	if (kinc_ios_gamepad_inited) return;
	kinc_ios_gamepad_inited = YES;

	// Attach to controllers already paired at launch
	for (GCController *c in [GCController controllers]) {
		kinc_ios_gamepad_register(c);
	}

	// Watch for connect / disconnect
	NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
	[nc addObserverForName:GCControllerDidConnectNotification
	                object:nil
	                 queue:[NSOperationQueue mainQueue]
	            usingBlock:^(NSNotification * _Nonnull note) {
		GCController *c = (GCController *)note.object;
		if (c) kinc_ios_gamepad_register(c);
	}];
	[nc addObserverForName:GCControllerDidDisconnectNotification
	                object:nil
	                 queue:[NSOperationQueue mainQueue]
	            usingBlock:^(NSNotification * _Nonnull note) {
		GCController *c = (GCController *)note.object;
		if (c) kinc_ios_gamepad_unregister(c);
	}];

	// Apple recommends starting controller discovery to also pick up
	// MFi/Bluetooth controllers paired after launch.  Available since
	// iOS 7; using the no-completion variant for simplicity.
	[GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}

const char *kinc_ios_gamepad_vendor_for(int slot) {
	if (slot < 0 || slot >= KINC_IOS_GAMEPAD_MAX) return "unknown";
	GCController *c = kinc_ios_gamepads[slot];
	if (c == nil) return "disconnected";
	NSString *name = c.vendorName != nil ? c.vendorName : @"unknown";
	const char *src = [name cStringUsingEncoding:NSUTF8StringEncoding];
	if (src == NULL) return "unknown";
	snprintf(kinc_ios_gamepad_vendor_buf[slot], sizeof(kinc_ios_gamepad_vendor_buf[slot]), "%s", src);
	return kinc_ios_gamepad_vendor_buf[slot];
}

const char *kinc_ios_gamepad_product_for(int slot) {
	if (slot < 0 || slot >= KINC_IOS_GAMEPAD_MAX) return "unknown";
	GCController *c = kinc_ios_gamepads[slot];
	if (c == nil) return "disconnected";
	NSString *name = nil;
	if (@available(iOS 13.0, *)) {
		name = c.productCategory;
	}
	if (name == nil) name = c.vendorName;
	if (name == nil) name = @"unknown";
	const char *src = [name cStringUsingEncoding:NSUTF8StringEncoding];
	if (src == NULL) return "unknown";
	snprintf(kinc_ios_gamepad_product_buf[slot], sizeof(kinc_ios_gamepad_product_buf[slot]), "%s", src);
	return kinc_ios_gamepad_product_buf[slot];
}

bool kinc_ios_gamepad_connected_for(int slot) {
	if (slot < 0 || slot >= KINC_IOS_GAMEPAD_MAX) return false;
	return kinc_ios_gamepads[slot] != nil;
}

#endif  // !KINC_NO_GAMEPAD_IOS
