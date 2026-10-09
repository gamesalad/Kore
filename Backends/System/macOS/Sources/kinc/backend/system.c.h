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

bool kinc_gamepad_connected(int num) {
	return true;
}

void kinc_gamepad_rumble(int gamepad, float left, float right) {}

// USB/HID vendor and product ids per gamepad index, written by
// HIDGamepad_bind/_unbind. They live here rather than in HIDGamepad.c.h so the
// accessors resolve whichever gamepad code is compiled in.
static int gamepad_hid_vendor_ids[KINC_GAMEPAD_MAX_COUNT];
static int gamepad_hid_product_ids[KINC_GAMEPAD_MAX_COUNT];
static bool gamepad_hid_ids_known[KINC_GAMEPAD_MAX_COUNT];

void kinc_macos_internal_gamepad_set_hid_ids(int gamepad, int vendor_id, int product_id) {
	if (gamepad < 0 || gamepad >= KINC_GAMEPAD_MAX_COUNT) {
		return;
	}
	gamepad_hid_ids_known[gamepad] = vendor_id >= 0 && product_id >= 0;
	gamepad_hid_vendor_ids[gamepad] = vendor_id;
	gamepad_hid_product_ids[gamepad] = product_id;
}

int kinc_gamepad_vendor_id(int gamepad) {
	if (gamepad < 0 || gamepad >= KINC_GAMEPAD_MAX_COUNT || !gamepad_hid_ids_known[gamepad]) {
		return -1;
	}
	return gamepad_hid_vendor_ids[gamepad];
}

int kinc_gamepad_product_id(int gamepad) {
	if (gamepad < 0 || gamepad >= KINC_GAMEPAD_MAX_COUNT || !gamepad_hid_ids_known[gamepad]) {
		return -1;
	}
	return gamepad_hid_product_ids[gamepad];
}
