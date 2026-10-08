#include <CoreAudio/AudioHardware.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <kinc/audio2/audio.h>
#include <kinc/backend/video.h>
#include <kinc/log.h>
#include <kinc/threads/mutex.h>

#include <stdio.h>
#include <string.h>

static kinc_internal_video_sound_stream_t *video = NULL;

void macPlayVideoSoundStream(kinc_internal_video_sound_stream_t *v) {
	video = v;
}

void macStopVideoSoundStream(void) {
	video = NULL;
}

static void affirm(OSStatus err) {
	if (err != kAudioHardwareNoError) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "Error: %i\n", err);
	}
}

// Output follows the system's default output device, or a device picked with kinc_a2_select_device. Everything that opens, closes or switches the
// device runs on device_queue (a serial queue that also receives the CoreAudio property notifications), so a caller never blocks on the IOProc - which
// matters because the IOProc runs the kinc_a2 callback under the audio mutex, and that callback may be waiting for the caller (a garbage collector
// stopping the world, for example). device_mutex only guards the device list and the selection, and is never held across a CoreAudio call that waits for
// the IOProc.

static bool initialized = false; // between kinc_a2_init and kinc_a2_shutdown
static bool created = false;     // the one-time setup (mutexes, buffer, queue) is done
static bool running = false;     // device_queue only: false after kinc_a2_shutdown, rebind_device does nothing then
static bool soundPlaying = false;
static AudioDeviceID device = kAudioDeviceUnknown;
static UInt32 deviceBufferSize;
static AudioStreamBasicDescription deviceFormat;
static AudioDeviceIOProcID theIOProcID = NULL;

static dispatch_queue_t device_queue = NULL;
static kinc_mutex_t device_mutex;
static CFStringRef selected_uid = NULL; // NULL: follow the system default

typedef struct {
	CFStringRef uid;
	char name[256];
} output_device_t;

static output_device_t *device_list = NULL;
static int device_list_count = 0;

static kinc_a2_buffer_t a2_buffer;

static uint32_t samples_per_second = 44100;

uint32_t kinc_a2_samples_per_second(void) {
	return samples_per_second;
}

static AudioObjectPropertyAddress property_address(AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
	AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
	return address;
}

static void read_sample(float *left, float *right) {
	*left = a2_buffer.channels[0][a2_buffer.read_location];
	*right = a2_buffer.channels[1][a2_buffer.read_location];
	a2_buffer.read_location += 1;
	if (a2_buffer.read_location >= a2_buffer.data_size) {
		a2_buffer.read_location = 0;
	}
}

static void zero_output(AudioBufferList *outOutputData) {
	for (UInt32 i = 0; i < outOutputData->mNumberBuffers; ++i) {
		if (outOutputData->mBuffers[i].mData != NULL) {
			memset(outOutputData->mBuffers[i].mData, 0, outOutputData->mBuffers[i].mDataByteSize);
		}
	}
}

static OSStatus appIOProc(AudioDeviceID inDevice, const AudioTimeStamp *inNow, const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                          AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime, void *userdata) {
	AudioStreamBasicDescription format;
	UInt32 size = sizeof(format);
	AudioObjectPropertyAddress address = property_address(kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeOutput);
	if (AudioObjectGetPropertyData(inDevice, &address, 0, NULL, &size, &format) != kAudioHardwareNoError) {
		format = deviceFormat;
	}
	if (format.mSampleRate > 0 && samples_per_second != (uint32_t)format.mSampleRate) {
		samples_per_second = (uint32_t)format.mSampleRate;
		kinc_a2_internal_sample_rate_callback();
	}

	if (outOutputData->mNumberBuffers == 0 || outOutputData->mBuffers[0].mData == NULL || outOutputData->mBuffers[0].mNumberChannels == 0 ||
	    !(format.mFormatFlags & kLinearPCMFormatFlagIsFloat)) {
		zero_output(outOutputData);
		return kAudioHardwareNoError;
	}

	// The buffer the device hands over decides the frame count, not the buffer size read when the device was opened: both change when the device is
	// reconfigured.
	UInt32 channels = outOutputData->mBuffers[0].mNumberChannels;
	UInt32 num_frames = outOutputData->mBuffers[0].mDataByteSize / (channels * sizeof(float));
	if (num_frames > a2_buffer.data_size) {
		num_frames = a2_buffer.data_size;
	}

	if (!kinc_a2_internal_callback(&a2_buffer, num_frames)) {
		zero_output(outOutputData);
		return kAudioHardwareNoError;
	}

	zero_output(outOutputData);
	bool deinterleaved = outOutputData->mNumberBuffers > 1 && channels == 1;
	if (deinterleaved) {
		float *left_out = (float *)outOutputData->mBuffers[0].mData;
		float *right_out = (float *)outOutputData->mBuffers[1].mData;
		UInt32 right_frames = right_out != NULL ? outOutputData->mBuffers[1].mDataByteSize / sizeof(float) : 0;
		for (UInt32 i = 0; i < num_frames; ++i) {
			float left, right;
			read_sample(&left, &right);
			left_out[i] = left;
			if (i < right_frames) {
				right_out[i] = right;
			}
		}
	}
	else {
		float *output = (float *)outOutputData->mBuffers[0].mData;
		for (UInt32 i = 0; i < num_frames; ++i) {
			float left, right;
			read_sample(&left, &right);
			if (channels == 1) {
				output[0] = (left + right) * 0.5f;
			}
			else {
				output[0] = left;
				output[1] = right;
			}
			output += channels;
		}
	}
	return kAudioHardwareNoError;
}

static bool device_has_output(AudioDeviceID id) {
	AudioObjectPropertyAddress address = property_address(kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput);
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(id, &address, 0, NULL, &size) != kAudioHardwareNoError || size == 0) {
		return false;
	}
	AudioBufferList *buffers = (AudioBufferList *)malloc(size);
	if (buffers == NULL) {
		return false;
	}
	bool has_output = false;
	if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, buffers) == kAudioHardwareNoError) {
		for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
			if (buffers->mBuffers[i].mNumberChannels > 0) {
				has_output = true;
				break;
			}
		}
	}
	free(buffers);
	return has_output;
}

static CFStringRef copy_device_string(AudioDeviceID id, AudioObjectPropertySelector selector) {
	AudioObjectPropertyAddress address = property_address(selector, kAudioObjectPropertyScopeGlobal);
	CFStringRef value = NULL;
	UInt32 size = sizeof(value);
	if (AudioObjectGetPropertyData(id, &address, 0, NULL, &size, &value) != kAudioHardwareNoError) {
		return NULL;
	}
	return value;
}

// All devices with at least one output channel. Returns a malloced array the caller frees, *count is set.
static AudioDeviceID *copy_output_devices(int *count) {
	*count = 0;
	AudioObjectPropertyAddress address = property_address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != kAudioHardwareNoError || size == 0) {
		return NULL;
	}
	AudioDeviceID *ids = (AudioDeviceID *)malloc(size);
	if (ids == NULL) {
		return NULL;
	}
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, ids) != kAudioHardwareNoError) {
		free(ids);
		return NULL;
	}
	int total = (int)(size / sizeof(AudioDeviceID));
	int outputs = 0;
	for (int i = 0; i < total; ++i) {
		if (device_has_output(ids[i])) {
			ids[outputs++] = ids[i];
		}
	}
	*count = outputs;
	return ids;
}

static AudioDeviceID find_output_device(CFStringRef uid) {
	int count = 0;
	AudioDeviceID *ids = copy_output_devices(&count);
	AudioDeviceID found = kAudioDeviceUnknown;
	for (int i = 0; i < count && found == kAudioDeviceUnknown; ++i) {
		CFStringRef device_uid = copy_device_string(ids[i], kAudioDevicePropertyDeviceUID);
		if (device_uid != NULL) {
			if (CFStringCompare(device_uid, uid, 0) == kCFCompareEqualTo) {
				found = ids[i];
			}
			CFRelease(device_uid);
		}
	}
	free(ids);
	return found;
}

static AudioDeviceID default_output_device(void) {
	AudioDeviceID id = kAudioDeviceUnknown;
	UInt32 size = sizeof(id);
	AudioObjectPropertyAddress address = property_address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
	affirm(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &id));
	return id;
}

// device_queue only. The device output is playing on, kAudioDeviceUnknown while silent.
static AudioDeviceID device_in_use(void) {
	return soundPlaying ? device : kAudioDeviceUnknown;
}

// device_queue only.
static void stop_device(void) {
	if (soundPlaying) {
		affirm(AudioDeviceStop(device, theIOProcID));
		affirm(AudioDeviceDestroyIOProcID(device, theIOProcID));
		theIOProcID = NULL;
		soundPlaying = false;
	}
}

// device_queue only. Opens new_device; on failure output stays silent until the next device change.
static bool start_device(AudioDeviceID new_device) {
	device = new_device;
	if (device == kAudioDeviceUnknown) {
		kinc_log(KINC_LOG_LEVEL_WARNING, "No audio output device.");
		return false;
	}

	UInt32 size = sizeof(UInt32);
	AudioObjectPropertyAddress address = property_address(kAudioDevicePropertyBufferSize, kAudioDevicePropertyScopeOutput);
	affirm(AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &deviceBufferSize));

	size = sizeof(AudioStreamBasicDescription);
	address = property_address(kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeOutput);
	affirm(AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &deviceFormat));

	CFStringRef name = copy_device_string(device, kAudioObjectPropertyName);
	char name_utf8[256] = "?";
	if (name != NULL) {
		CFStringGetCString(name, name_utf8, sizeof(name_utf8), kCFStringEncodingUTF8);
		CFRelease(name);
	}
	kinc_log(KINC_LOG_LEVEL_INFO, "Audio output device: %s (id %u)\n", name_utf8, (unsigned int)device);
	kinc_log(KINC_LOG_LEVEL_INFO, "deviceBufferSize = %i\n", deviceBufferSize);

	if (deviceFormat.mFormatID != kAudioFormatLinearPCM) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "mFormatID !=  kAudioFormatLinearPCM\n");
		return false;
	}

	if (!(deviceFormat.mFormatFlags & kLinearPCMFormatFlagIsFloat)) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "Only works with float format.\n");
		return false;
	}

	if (samples_per_second != (uint32_t)deviceFormat.mSampleRate) {
		samples_per_second = (uint32_t)deviceFormat.mSampleRate;
		kinc_a2_internal_sample_rate_callback();
	}

	kinc_log(KINC_LOG_LEVEL_INFO, "mSampleRate = %g\n", deviceFormat.mSampleRate);
	kinc_log(KINC_LOG_LEVEL_INFO, "mFormatFlags = %08X\n", (unsigned int)deviceFormat.mFormatFlags);
	kinc_log(KINC_LOG_LEVEL_INFO, "mBytesPerPacket = %d\n", (unsigned int)deviceFormat.mBytesPerPacket);
	kinc_log(KINC_LOG_LEVEL_INFO, "mFramesPerPacket = %d\n", (unsigned int)deviceFormat.mFramesPerPacket);
	kinc_log(KINC_LOG_LEVEL_INFO, "mChannelsPerFrame = %d\n", (unsigned int)deviceFormat.mChannelsPerFrame);
	kinc_log(KINC_LOG_LEVEL_INFO, "mBytesPerFrame = %d\n", (unsigned int)deviceFormat.mBytesPerFrame);
	kinc_log(KINC_LOG_LEVEL_INFO, "mBitsPerChannel = %d\n", (unsigned int)deviceFormat.mBitsPerChannel);

	OSStatus err = AudioDeviceCreateIOProcID(device, appIOProc, NULL, &theIOProcID);
	if (err != kAudioHardwareNoError) {
		affirm(err);
		theIOProcID = NULL;
		return false;
	}
	err = AudioDeviceStart(device, theIOProcID);
	if (err != kAudioHardwareNoError) {
		affirm(err);
		affirm(AudioDeviceDestroyIOProcID(device, theIOProcID));
		theIOProcID = NULL;
		return false;
	}
	soundPlaying = true;
	return true;
}

// device_queue only. Drops the selection when it is still uid.
static void drop_selection(CFStringRef uid) {
	kinc_mutex_lock(&device_mutex);
	if (selected_uid != NULL && CFStringCompare(selected_uid, uid, 0) == kCFCompareEqualTo) {
		CFRelease(selected_uid);
		selected_uid = NULL;
	}
	kinc_mutex_unlock(&device_mutex);
}

// device_queue only. Moves output to the device it should be on now: the selected one, or the system default when nothing is selected or the selected
// device is gone or can not be opened (both drop the selection). Nothing happens when output is already there. The device-changed callback fires when
// the device in use changed (a device that failed to open is not in use, so retrying it on every notification reports nothing) or the selection was
// dropped.
static void rebind_device(void) {
	if (!running) {
		return;
	}

	kinc_mutex_lock(&device_mutex);
	CFStringRef uid = selected_uid != NULL ? (CFStringRef)CFRetain(selected_uid) : NULL;
	kinc_mutex_unlock(&device_mutex);

	AudioDeviceID target = kAudioDeviceUnknown;
	bool fell_back = false;
	if (uid != NULL) {
		target = find_output_device(uid);
		if (target == kAudioDeviceUnknown) {
			fell_back = true;
			kinc_log(KINC_LOG_LEVEL_WARNING, "The selected audio output device is gone, following the system default.");
			drop_selection(uid);
		}
	}
	bool selected = target != kAudioDeviceUnknown;
	if (target == kAudioDeviceUnknown) {
		target = default_output_device();
	}

	if (target == device && soundPlaying) {
		if (uid != NULL) {
			CFRelease(uid);
		}
		if (fell_back) {
			kinc_a2_internal_device_changed_callback();
		}
		return;
	}

	AudioDeviceID previous = device_in_use();
	stop_device();
	if (!start_device(target) && selected) {
		fell_back = true;
		kinc_log(KINC_LOG_LEVEL_WARNING, "The selected audio output device can not be opened, following the system default.");
		drop_selection(uid);
		AudioDeviceID fallback = default_output_device();
		if (fallback != target) {
			start_device(fallback);
		}
	}
	if (uid != NULL) {
		CFRelease(uid);
	}
	if (device_in_use() != previous || fell_back) {
		kinc_a2_internal_device_changed_callback();
	}
}

static OSStatus device_listener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses, void *userdata) {
	// CoreAudio's notification thread: hop to device_queue, never switch devices here.
	dispatch_async(device_queue, ^{
	  rebind_device();
	});
	return kAudioHardwareNoError;
}

static const AudioObjectPropertySelector listened_properties[] = {kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyDevices};

static void free_device_list(void) {
	for (int i = 0; i < device_list_count; ++i) {
		if (device_list[i].uid != NULL) {
			CFRelease(device_list[i].uid);
		}
	}
	free(device_list);
	device_list = NULL;
	device_list_count = 0;
}

void kinc_a2_init(void) {
	if (initialized) {
		return;
	}

	bool first = !created;
	if (first) {
		created = true;
		kinc_a2_internal_init();
		kinc_mutex_init(&device_mutex);

		a2_buffer.read_location = 0;
		a2_buffer.write_location = 0;
		a2_buffer.data_size = 128 * 1024;
		a2_buffer.channel_count = 2;
		a2_buffer.channels[0] = (float *)calloc(a2_buffer.data_size, sizeof(float));
		a2_buffer.channels[1] = (float *)calloc(a2_buffer.data_size, sizeof(float));

		device_queue = dispatch_queue_create("kinc.audio2.device", DISPATCH_QUEUE_SERIAL);
	}
	initialized = true;

	for (size_t i = 0; i < sizeof(listened_properties) / sizeof(listened_properties[0]); ++i) {
		AudioObjectPropertyAddress address = property_address(listened_properties[i], kAudioObjectPropertyScopeGlobal);
		affirm(AudioObjectAddPropertyListener(kAudioObjectSystemObject, &address, device_listener, NULL));
	}

	void (^start)(void) = ^{
	  running = true;
	  start_device(default_output_device());
	};
	if (first) {
		// Synchronous: no kinc_a2 callback can be set yet, so the first device is open when kinc_a2_init returns, like before.
		dispatch_sync(device_queue, start);
	}
	else {
		// After kinc_a2_shutdown a callback may be set, and the stop queued by the shutdown may still be waiting for it - never wait here.
		dispatch_async(device_queue, start);
	}
}

void kinc_a2_update(void) {}

// Stops output and drops the device selection and list. It does not wait for the device to stop: the IOProc may be running the kinc_a2 callback, and
// that may be waiting for this thread (a garbage collector stopping the world), while AudioDeviceStop waits for the IOProc. So the callback can run once
// more after this returns. kinc_a2_init starts output again.
void kinc_a2_shutdown(void) {
	if (!initialized) {
		return;
	}
	initialized = false;
	for (size_t i = 0; i < sizeof(listened_properties) / sizeof(listened_properties[0]); ++i) {
		AudioObjectPropertyAddress address = property_address(listened_properties[i], kAudioObjectPropertyScopeGlobal);
		affirm(AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &address, device_listener, NULL));
	}
	kinc_mutex_lock(&device_mutex);
	if (selected_uid != NULL) {
		CFRelease(selected_uid);
		selected_uid = NULL;
	}
	free_device_list();
	kinc_mutex_unlock(&device_mutex);
	dispatch_async(device_queue, ^{
	  running = false;
	  stop_device();
	});
}

int kinc_a2_device_count(void) {
	if (!initialized) {
		return 0;
	}
	int count = 0;
	AudioDeviceID *ids = copy_output_devices(&count);
	output_device_t *list = count > 0 ? (output_device_t *)calloc(count, sizeof(output_device_t)) : NULL;
	int listed = 0;
	for (int i = 0; i < count && list != NULL; ++i) {
		CFStringRef uid = copy_device_string(ids[i], kAudioDevicePropertyDeviceUID);
		if (uid == NULL) {
			continue;
		}
		list[listed].uid = uid;
		CFStringRef name = copy_device_string(ids[i], kAudioObjectPropertyName);
		if (name == NULL || !CFStringGetCString(name, list[listed].name, sizeof(list[listed].name), kCFStringEncodingUTF8)) {
			snprintf(list[listed].name, sizeof(list[listed].name), "Audio device %u", (unsigned int)ids[i]);
		}
		if (name != NULL) {
			CFRelease(name);
		}
		++listed;
	}
	free(ids);

	kinc_mutex_lock(&device_mutex);
	free_device_list();
	device_list = list;
	device_list_count = listed;
	kinc_mutex_unlock(&device_mutex);
	return listed;
}

const char *kinc_a2_device_name(int index) {
	if (!initialized) {
		return NULL;
	}
	kinc_mutex_lock(&device_mutex);
	const char *name = index >= 0 && index < device_list_count ? device_list[index].name : NULL;
	kinc_mutex_unlock(&device_mutex);
	return name;
}

bool kinc_a2_select_device(int index) {
	if (!initialized) {
		return false;
	}
	kinc_mutex_lock(&device_mutex);
	bool valid = index == KINC_A2_DEFAULT_DEVICE || (index >= 0 && index < device_list_count);
	if (valid) {
		if (selected_uid != NULL) {
			CFRelease(selected_uid);
			selected_uid = NULL;
		}
		if (index != KINC_A2_DEFAULT_DEVICE) {
			selected_uid = (CFStringRef)CFRetain(device_list[index].uid);
		}
	}
	kinc_mutex_unlock(&device_mutex);
	if (valid) {
		dispatch_async(device_queue, ^{
		  rebind_device();
		});
	}
	return valid;
}

int kinc_a2_selected_device(void) {
	if (!initialized) {
		return KINC_A2_DEFAULT_DEVICE;
	}
	int index = KINC_A2_DEFAULT_DEVICE;
	kinc_mutex_lock(&device_mutex);
	if (selected_uid != NULL) {
		for (int i = 0; i < device_list_count; ++i) {
			if (CFStringCompare(device_list[i].uid, selected_uid, 0) == kCFCompareEqualTo) {
				index = i;
				break;
			}
		}
	}
	kinc_mutex_unlock(&device_mutex);
	return index;
}
