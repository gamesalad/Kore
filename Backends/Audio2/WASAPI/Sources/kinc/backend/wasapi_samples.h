#pragma once

// How wasapi.c writes a sample in the device's mix format. Free of Windows headers so it can be tested on any host.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
	KINC_WASAPI_SAMPLES_UNSUPPORTED, // written as silence
	KINC_WASAPI_SAMPLES_U8,
	KINC_WASAPI_SAMPLES_S16,
	KINC_WASAPI_SAMPLES_S24, // packed, three bytes
	KINC_WASAPI_SAMPLES_S32,
	KINC_WASAPI_SAMPLES_F32
} kinc_wasapi_samples_t;

#define KINC_WASAPI_TAG_PCM 1        // WAVE_FORMAT_PCM
#define KINC_WASAPI_TAG_IEEE_FLOAT 3 // WAVE_FORMAT_IEEE_FLOAT

// tag is KINC_WASAPI_TAG_PCM or KINC_WASAPI_TAG_IEEE_FLOAT - for WAVE_FORMAT_EXTENSIBLE the one its SubFormat names - or anything else for a format that
// can not be written. bits_per_sample is the container size (wBitsPerSample): a 32-bit container with 24 valid bits takes left-aligned 32-bit samples.
// A frame (block_align bytes) must hold the first one or two channels.
static kinc_wasapi_samples_t kinc_wasapi_samples_of(unsigned tag, unsigned bits_per_sample, unsigned channels, unsigned block_align) {
	kinc_wasapi_samples_t samples = KINC_WASAPI_SAMPLES_UNSUPPORTED;
	if (tag == KINC_WASAPI_TAG_PCM) {
		switch (bits_per_sample) {
		case 8:
			samples = KINC_WASAPI_SAMPLES_U8;
			break;
		case 16:
			samples = KINC_WASAPI_SAMPLES_S16;
			break;
		case 24:
			samples = KINC_WASAPI_SAMPLES_S24;
			break;
		case 32:
			samples = KINC_WASAPI_SAMPLES_S32;
			break;
		}
	}
	else if (tag == KINC_WASAPI_TAG_IEEE_FLOAT && bits_per_sample == 32) {
		samples = KINC_WASAPI_SAMPLES_F32;
	}
	unsigned written = channels < 2 ? channels : 2;
	if (written == 0 || block_align < written * (bits_per_sample / 8)) {
		samples = KINC_WASAPI_SAMPLES_UNSUPPORTED;
	}
	return samples;
}

static unsigned kinc_wasapi_sample_bytes(kinc_wasapi_samples_t samples) {
	switch (samples) {
	case KINC_WASAPI_SAMPLES_U8:
		return 1;
	case KINC_WASAPI_SAMPLES_S16:
		return 2;
	case KINC_WASAPI_SAMPLES_S24:
		return 3;
	case KINC_WASAPI_SAMPLES_S32:
	case KINC_WASAPI_SAMPLES_F32:
		return 4;
	default:
		return 0;
	}
}

// Fills bytes with silence: 0x80 for unsigned 8-bit samples, zero for everything else.
static void kinc_wasapi_write_silence(uint8_t *buffer, size_t bytes, kinc_wasapi_samples_t samples) {
	memset(buffer, samples == KINC_WASAPI_SAMPLES_U8 ? 0x80 : 0, bytes);
}

// Writes value (clamped to [-1, 1]) as one little-endian sample at out, which needs no alignment.
static void kinc_wasapi_write_sample(uint8_t *out, kinc_wasapi_samples_t samples, float value) {
	if (value != value) { // NaN
		value = 0.0f;
	}
	else if (value > 1.0f) {
		value = 1.0f;
	}
	else if (value < -1.0f) {
		value = -1.0f;
	}
	switch (samples) {
	case KINC_WASAPI_SAMPLES_U8:
		out[0] = (uint8_t)(128 + (int)(value * 127));
		break;
	case KINC_WASAPI_SAMPLES_S16: {
		int16_t sample = (int16_t)(value * 32767);
		out[0] = (uint8_t)(sample & 0xff);
		out[1] = (uint8_t)((sample >> 8) & 0xff);
		break;
	}
	case KINC_WASAPI_SAMPLES_S24: {
		int32_t sample = (int32_t)(value * 8388607);
		out[0] = (uint8_t)(sample & 0xff);
		out[1] = (uint8_t)((sample >> 8) & 0xff);
		out[2] = (uint8_t)((sample >> 16) & 0xff);
		break;
	}
	case KINC_WASAPI_SAMPLES_S32: {
		int32_t sample = (int32_t)((double)value * 2147483647.0);
		out[0] = (uint8_t)(sample & 0xff);
		out[1] = (uint8_t)((sample >> 8) & 0xff);
		out[2] = (uint8_t)((sample >> 16) & 0xff);
		out[3] = (uint8_t)((sample >> 24) & 0xff);
		break;
	}
	case KINC_WASAPI_SAMPLES_F32:
		memcpy(out, &value, sizeof(float)); // Windows is little-endian
		break;
	default:
		break;
	}
}
