#include <kinc/graphics4/graphics.h>
#include <kinc/input/gamepad.h>
#include <kinc/input/keyboard.h>
#include <kinc/input/mouse.h>
#include <kinc/system.h>
#include <kinc/video.h>

static int mouseX, mouseY;
static bool keyboardShown = false;

void Kinc_Mouse_GetPosition(int window, int *x, int *y) {
	*x = mouseX;
	*y = mouseY;
}

void kinc_keyboard_show(void) {
	keyboardShown = true;
}

void kinc_keyboard_hide(void) {
	keyboardShown = false;
}

bool kinc_keyboard_active(void) {
	return keyboardShown;
}

void kinc_vibrate(int ms) {}

const char *kinc_system_id(void) {
	return "macOS";
}

static const char *videoFormats[] = {"ogv", NULL};

const char **kinc_video_formats(void) {
	return videoFormats;
}

void kinc_set_keep_screen_on(bool on) {}

#include <mach/mach_time.h>

double kinc_frequency(void) {
	mach_timebase_info_data_t info;
	mach_timebase_info(&info);
	return (double)info.denom / (double)info.numer / 1e-9;
}

kinc_ticks_t kinc_timestamp(void) {
	return mach_absolute_time();
}

void kinc_login(void) {}

void kinc_unlock_achievement(int id) {}

// kinc_gamepad_connected lives in HIDGamepad.c.h — answers from real
// HIDManager state instead of a hardcoded `true` (which produced
// phantom slot connect events on the Kha-side polling loop).
//
// Opt-out stubs for KINC_NO_GAMEPAD_MACOS — when the HID TUs are
// compiled out, the kinc_gamepad_{vendor,product_name,connected}
// symbols must still resolve at link time. Mirror the iOS opt-out
// stubs in iOS/system.m.h; "nobody"/"none" are the sentinels the
// engine-side phantom filter ignores.
#ifdef KINC_NO_GAMEPAD_MACOS
const char *kinc_gamepad_vendor(int gamepad)       { return "nobody"; }
const char *kinc_gamepad_product_name(int gamepad) { return "none"; }
bool        kinc_gamepad_connected(int num)        { return false; }
#endif

void kinc_gamepad_rumble(int gamepad, float left, float right) {}
