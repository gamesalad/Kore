#pragma once

// Output-device selection for audio backends that can only play on the system's default device: one device named "Default", selection is a no-op.
// Included by those backends' unit files; backends with real device selection (macOS, WASAPI) implement these functions themselves.

#include <kinc/audio2/audio.h>

#include <stddef.h>

int kinc_a2_device_count(void) {
	return 1;
}

const char *kinc_a2_device_name(int index) {
	return index == 0 ? "Default" : NULL;
}

bool kinc_a2_select_device(int index) {
	return index == KINC_A2_DEFAULT_DEVICE || index == 0;
}

int kinc_a2_selected_device(void) {
	return KINC_A2_DEFAULT_DEVICE;
}
