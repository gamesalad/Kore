#include <kinc/input/gamepad.h>

const char *kinc_gamepad_vendor(int gamepad) {
	return "None";
}

const char *kinc_gamepad_product_name(int gamepad) {
	return "Gamepad";
}

// No gamepad support here.
int kinc_gamepad_vendor_id(int gamepad) {
	return -1;
}

int kinc_gamepad_product_id(int gamepad) {
	return -1;
}

bool kinc_gamepad_connected(int gamepad) {
	return false;
}

void kinc_gamepad_rumble(int gamepad, float left, float right) {}
