#include <kinc/audio2/audio.h>

#include <kinc/backend/SystemMicrosoft.h>

#include <kinc/error.h>
#include <kinc/log.h>
#include <kinc/threads/mutex.h>

// Windows 7
#define WINVER 0x0601
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0601

#define NOATOM
#define NOCLIPBOARD
#define NOCOLOR
#define NOCOMM
#define NOCTLMGR
#define NODEFERWINDOWPOS
#define NODRAWTEXT
// #define NOGDI
#define NOGDICAPMASKS
#define NOHELP
#define NOICONS
#define NOKANJI
#define NOKEYSTATES
#define NOMB
#define NOMCX
#define NOMEMMGR
#define NOMENUS
#define NOMETAFILE
#define NOMINMAX
// #define NOMSG
#define NOOPENFILE
#define NOPROFILER
#define NORASTEROPS
#define NOSCROLL
#define NOSERVICE
#define NOSHOWWINDOW
#define NOSOUND
#define NOSYSCOMMANDS
#define NOSYSMETRICS
#define NOTEXTMETRIC
// #define NOUSER
#define NOVIRTUALKEYCODES
#define NOWH
#define NOWINMESSAGES
#define NOWINOFFSETS
#define NOWINSTYLES
#define WIN32_LEAN_AND_MEAN

#include <initguid.h>

#include <AudioClient.h>
#include <mmdeviceapi.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef __MINGW32__
// MIDL_INTERFACE("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2")
DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
// MIDL_INTERFACE("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")
DEFINE_GUID(IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
// MIDL_INTERFACE("A95664D2-9614-4F35-A746-DE8DB63617E6")
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
// DECLSPEC_UUID("BCDE0395-E52F-467C-8E3D-C4579291692E")
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
#endif
// Own names so they never collide with a definition from the SDK's libraries.
// MIDL_INTERFACE("7991EEC9-7E89-4D85-8390-6C703CEC60C0")
DEFINE_GUID(kinc_IID_IMMNotificationClient, 0x7991EEC9, 0x7E89, 0x4D85, 0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0);
// 00000000-0000-0000-C000-000000000046
DEFINE_GUID(kinc_IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
// PKEY_Device_FriendlyName
static const PROPERTYKEY kinc_PKEY_Device_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// based on the implementation in soloud and Microsoft sample code
static kinc_a2_buffer_t a2_buffer;

static IMMDeviceEnumerator *deviceEnumerator;
static IMMDevice *device = NULL;
static IAudioClient *audioClient = NULL;
static IAudioRenderClient *renderClient = NULL;
static HANDLE bufferEndEvent = 0;
static UINT32 bufferFrames;
static WAVEFORMATEX requestedFormat;
static WAVEFORMATEX *format;
static WAVEFORMATEX *allocatedFormat = NULL;
static uint32_t samples_per_second = 44100;

// Output follows the system's default render device (eRender, eConsole), or a device picked with kinc_a2_select_device. Devices are only ever opened,
// switched and closed on the audio thread: the IMMNotificationClient callbacks and kinc_a2_select_device just raise reinitRequested and wake the thread
// through reinitEvent. device_mutex guards the device list and the selection; it is never held while the kinc_a2 callback runs.
static kinc_mutex_t device_mutex;
static wchar_t *selected_id = NULL; // NULL: follow the system default
static wchar_t *current_id = NULL;  // the device that is open, audio thread only
static HANDLE reinitEvent = 0;
static volatile LONG reinitRequested = 0;

typedef struct {
	wchar_t *id;
	char name[256];
} output_device_t;

static output_device_t *device_list = NULL;
static int device_list_count = 0;

uint32_t kinc_a2_samples_per_second(void) {
	return samples_per_second;
}

static wchar_t *copy_wstring(const wchar_t *source) {
	if (source == NULL) {
		return NULL;
	}
	size_t length = wcslen(source);
	wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
	if (copy != NULL) {
		memcpy(copy, source, (length + 1) * sizeof(wchar_t));
	}
	return copy;
}

static void requestReinit(void) {
	InterlockedExchange(&reinitRequested, 1);
	if (reinitEvent != 0) {
		SetEvent(reinitEvent);
	}
}

static void releaseDevice(void) {
	if (audioClient != NULL) {
		audioClient->lpVtbl->Stop(audioClient);
	}

	if (renderClient != NULL) {
		renderClient->lpVtbl->Release(renderClient);
		renderClient = NULL;
	}

	if (audioClient != NULL) {
		audioClient->lpVtbl->Release(audioClient);
		audioClient = NULL;
	}

	if (device != NULL) {
		device->lpVtbl->Release(device);
		device = NULL;
	}

	if (bufferEndEvent != 0) {
		CloseHandle(bufferEndEvent);
		bufferEndEvent = 0;
	}

	if (allocatedFormat != NULL) {
		CoTaskMemFree(allocatedFormat);
		allocatedFormat = NULL;
	}
	format = NULL;
}

// Drops the selection when it is still id.
static void dropSelection(const wchar_t *id) {
	kinc_mutex_lock(&device_mutex);
	if (selected_id != NULL && id != NULL && wcscmp(selected_id, id) == 0) {
		free(selected_id);
		selected_id = NULL;
	}
	kinc_mutex_unlock(&device_mutex);
}

static IMMDevice *defaultDevice(void) {
	IMMDevice *found = NULL;
	if (deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(deviceEnumerator, eRender, eConsole, &found) != S_OK) {
		found = NULL;
	}
	return found;
}

// The selected device when it is still active (*selected is set), otherwise the default render device. *fell_back is set when a selected device could
// not be used, which also drops the selection.
static IMMDevice *findDevice(bool *fell_back, bool *selected) {
	*fell_back = false;
	*selected = false;

	kinc_mutex_lock(&device_mutex);
	wchar_t *wanted = copy_wstring(selected_id);
	kinc_mutex_unlock(&device_mutex);

	IMMDevice *found = NULL;
	if (wanted != NULL) {
		DWORD state = 0;
		if (deviceEnumerator->lpVtbl->GetDevice(deviceEnumerator, wanted, &found) != S_OK || found->lpVtbl->GetState(found, &state) != S_OK ||
		    state != DEVICE_STATE_ACTIVE) {
			if (found != NULL) {
				found->lpVtbl->Release(found);
				found = NULL;
			}
			*fell_back = true;
			kinc_log(KINC_LOG_LEVEL_WARNING, "The selected audio output device is gone, following the system default.");
			dropSelection(wanted);
		}
		else {
			*selected = true;
		}
		free(wanted);
	}

	if (found == NULL) {
		found = defaultDevice();
	}
	return found;
}

static wchar_t *deviceId(IMMDevice *dev) {
	LPWSTR id = NULL;
	if (dev == NULL || dev->lpVtbl->GetId(dev, &id) != S_OK || id == NULL) {
		return NULL;
	}
	wchar_t *copy = copy_wstring(id);
	CoTaskMemFree(id);
	return copy;
}

// Opens dev (takes the reference). Returns false and stays silent when it can not be opened.
static bool initDevice(IMMDevice *dev) {
	releaseDevice();

	kinc_log(KINC_LOG_LEVEL_INFO, "Initializing a new audio device.");

	device = dev;
	if (device == NULL) {
		kinc_log(KINC_LOG_LEVEL_WARNING, "Could not initialize WASAPI audio: no output device.");
		return false;
	}

	HRESULT hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, 0, (void **)&audioClient);
	if (hr != S_OK) {
		audioClient = NULL;
		kinc_log(KINC_LOG_LEVEL_WARNING, "Could not initialize WASAPI audio.");
		return false;
	}

	const int sampleRate = 48000;

	format = &requestedFormat;
	memset(&requestedFormat, 0, sizeof(WAVEFORMATEX));
	requestedFormat.nChannels = 2;
	requestedFormat.nSamplesPerSec = sampleRate;
	requestedFormat.wFormatTag = WAVE_FORMAT_PCM;
	requestedFormat.wBitsPerSample = sizeof(short) * 8;
	requestedFormat.nBlockAlign = (requestedFormat.nChannels * requestedFormat.wBitsPerSample) / 8;
	requestedFormat.nAvgBytesPerSec = requestedFormat.nSamplesPerSec * requestedFormat.nBlockAlign;
	requestedFormat.cbSize = 0;

	WAVEFORMATEX *closestFormat = NULL;
	HRESULT supported = audioClient->lpVtbl->IsFormatSupported(audioClient, AUDCLNT_SHAREMODE_SHARED, format, &closestFormat);
	if (supported != S_OK) {
		kinc_log(KINC_LOG_LEVEL_WARNING, "Falling back to the system's preferred WASAPI mix format.");
		if (closestFormat != NULL) {
			allocatedFormat = closestFormat;
			format = closestFormat;
		}
		else if (audioClient->lpVtbl->GetMixFormat(audioClient, &allocatedFormat) == S_OK && allocatedFormat != NULL) {
			format = allocatedFormat;
		}
	}
	else if (closestFormat != NULL) {
		CoTaskMemFree(closestFormat);
	}

	HRESULT result =
	    audioClient->lpVtbl->Initialize(audioClient, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 40 * 1000 * 10, 0, format, 0);
	if (result != S_OK) {
		kinc_log(KINC_LOG_LEVEL_WARNING, "Could not initialize WASAPI audio, going silent (error code 0x%x).", result);
		releaseDevice();
		return false;
	}

	uint32_t old_samples_per_second = samples_per_second;
	samples_per_second = format->nSamplesPerSec;
	if (samples_per_second != old_samples_per_second) {
		kinc_a2_internal_sample_rate_callback();
	}
	a2_buffer.channel_count = 2;

	bufferFrames = 0;
	kinc_microsoft_affirm(audioClient->lpVtbl->GetBufferSize(audioClient, &bufferFrames));
	if (audioClient->lpVtbl->GetService(audioClient, &IID_IAudioRenderClient, (void **)&renderClient) != S_OK) {
		renderClient = NULL;
		kinc_log(KINC_LOG_LEVEL_WARNING, "Could not get the WASAPI render client, going silent.");
		releaseDevice();
		return false;
	}

	bufferEndEvent = CreateEvent(0, FALSE, FALSE, 0);
	kinc_affirm(bufferEndEvent != 0);

	kinc_microsoft_affirm(audioClient->lpVtbl->SetEventHandle(audioClient, bufferEndEvent));

	return true;
}

static void submitEmptyBuffer(unsigned frames) {
	BYTE *buffer = NULL;
	HRESULT result = renderClient->lpVtbl->GetBuffer(renderClient, frames, &buffer);
	if (FAILED(result)) {
		return;
	}

	memset(buffer, 0, frames * format->nBlockAlign);

	result = renderClient->lpVtbl->ReleaseBuffer(renderClient, frames, 0);
}

static bool sameId(const wchar_t *a, const wchar_t *b) {
	return a == NULL ? b == NULL : (b != NULL && wcscmp(a, b) == 0);
}

// Audio thread only. Moves output to the device it should be on now; with force it reopens even the same device (after AUDCLNT_E_DEVICE_INVALIDATED).
// The device-changed callback fires when the device in use changed (one that failed to open is not in use, so the retries while there is no device
// report nothing) or the selection was dropped.
static void reinitAudio(bool force) {
	bool fell_back = false;
	bool selected = false;
	IMMDevice *target = findDevice(&fell_back, &selected);
	wchar_t *target_id = deviceId(target);

	if (!force && audioClient != NULL && target_id != NULL && current_id != NULL && wcscmp(target_id, current_id) == 0) {
		if (target != NULL) {
			target->lpVtbl->Release(target);
		}
		free(target_id);
		if (fell_back) {
			kinc_a2_internal_device_changed_callback();
		}
		return;
	}

	wchar_t *previous_id = audioClient != NULL ? copy_wstring(current_id) : NULL;
	free(current_id);
	current_id = target_id;

	if (initDevice(target)) {
		submitEmptyBuffer(bufferFrames);
		audioClient->lpVtbl->Start(audioClient);
	}
	else if (selected) {
		fell_back = true;
		kinc_log(KINC_LOG_LEVEL_WARNING, "The selected audio output device can not be opened, following the system default.");
		dropSelection(current_id);
		IMMDevice *fallback = defaultDevice();
		wchar_t *fallback_id = deviceId(fallback);
		if (fallback != NULL && !sameId(fallback_id, current_id)) {
			free(current_id);
			current_id = fallback_id;
			if (initDevice(fallback)) {
				submitEmptyBuffer(bufferFrames);
				audioClient->lpVtbl->Start(audioClient);
			}
		}
		else {
			if (fallback != NULL) {
				fallback->lpVtbl->Release(fallback);
			}
			free(fallback_id);
		}
	}

	bool changed = fell_back || !sameId(previous_id, audioClient != NULL ? current_id : NULL);
	free(previous_id);
	if (changed) {
		kinc_a2_internal_device_changed_callback();
	}
}

static void restartAudio(void) {
	reinitAudio(true);
}

static void readSample(float *left, float *right) {
	*left = a2_buffer.channels[0][a2_buffer.read_location];
	*right = a2_buffer.channels[1][a2_buffer.read_location];
	a2_buffer.read_location += 1;
	if (a2_buffer.read_location >= a2_buffer.data_size) {
		a2_buffer.read_location = 0;
	}
}

static void submitBuffer(unsigned frames) {
	BYTE *buffer = NULL;
	HRESULT result = renderClient->lpVtbl->GetBuffer(renderClient, frames, &buffer);
	if (FAILED(result)) {
		if (result == AUDCLNT_E_DEVICE_INVALIDATED) {
			restartAudio();
		}
		return;
	}

	// A mono format gets left and right mixed down; channels past the first two (a surround mix format) stay silent.
	memset(buffer, 0, frames * format->nBlockAlign);
	if (kinc_a2_internal_callback(&a2_buffer, frames)) {
		bool mono = format->nChannels < 2;
		if (format->wFormatTag == WAVE_FORMAT_PCM) {
			for (UINT32 i = 0; i < frames; ++i) {
				float left, right;
				readSample(&left, &right);
				int16_t *out = (int16_t *)&buffer[i * format->nBlockAlign];
				if (mono) {
					out[0] = (int16_t)((left + right) * 0.5f * 32767);
				}
				else {
					out[0] = (int16_t)(left * 32767);
					out[1] = (int16_t)(right * 32767);
				}
			}
		}
		else {
			for (UINT32 i = 0; i < frames; ++i) {
				float left, right;
				readSample(&left, &right);
				float *out = (float *)&buffer[i * format->nBlockAlign];
				if (mono) {
					out[0] = (left + right) * 0.5f;
				}
				else {
					out[0] = left;
					out[1] = right;
				}
			}
		}
	}

	result = renderClient->lpVtbl->ReleaseBuffer(renderClient, frames, 0);
	if (FAILED(result)) {
		if (result == AUDCLNT_E_DEVICE_INVALIDATED) {
			restartAudio();
		}
	}
}

static DWORD WINAPI audioThread(LPVOID ignored) {
	if (audioClient != NULL) {
		submitBuffer(bufferFrames);
		audioClient->lpVtbl->Start(audioClient);
	}
	while (1) {
		if (audioClient == NULL) {
			// No device open: wait for a device notification, and retry now and then.
			WaitForSingleObject(reinitEvent, 2000);
			InterlockedExchange(&reinitRequested, 0);
			reinitAudio(false);
			continue;
		}

		HANDLE events[2] = {bufferEndEvent, reinitEvent};
		WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (InterlockedExchange(&reinitRequested, 0) != 0) {
			reinitAudio(false);
			continue;
		}

		UINT32 padding = 0;
		HRESULT result = audioClient->lpVtbl->GetCurrentPadding(audioClient, &padding);
		if (FAILED(result)) {
			if (result == AUDCLNT_E_DEVICE_INVALIDATED) {
				restartAudio();
			}
			continue;
		}
		UINT32 frames = bufferFrames - padding;
		submitBuffer(frames);
	}
	return 0;
}

// IMMNotificationClient: called on a system thread, must not block - it only wakes the audio thread, which works out whether the device to play on
// changed.

static HRESULT STDMETHODCALLTYPE notification_QueryInterface(IMMNotificationClient *This, REFIID riid, void **ppvObject) {
	if (IsEqualIID(riid, &kinc_IID_IUnknown) || IsEqualIID(riid, &kinc_IID_IMMNotificationClient)) {
		*ppvObject = This;
		return S_OK;
	}
	*ppvObject = NULL;
	return E_NOINTERFACE;
}

// The client is a static object, so reference counting is a formality.
static ULONG STDMETHODCALLTYPE notification_AddRef(IMMNotificationClient *This) {
	return 1;
}

static ULONG STDMETHODCALLTYPE notification_Release(IMMNotificationClient *This) {
	return 1;
}

static HRESULT STDMETHODCALLTYPE notification_OnDeviceStateChanged(IMMNotificationClient *This, LPCWSTR pwstrDeviceId, DWORD dwNewState) {
	requestReinit();
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE notification_OnDeviceAdded(IMMNotificationClient *This, LPCWSTR pwstrDeviceId) {
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE notification_OnDeviceRemoved(IMMNotificationClient *This, LPCWSTR pwstrDeviceId) {
	requestReinit();
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE notification_OnDefaultDeviceChanged(IMMNotificationClient *This, EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) {
	if (flow == eRender && role == eConsole) {
		requestReinit();
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE notification_OnPropertyValueChanged(IMMNotificationClient *This, LPCWSTR pwstrDeviceId, const PROPERTYKEY key) {
	return S_OK;
}

static IMMNotificationClientVtbl notificationVtbl = {
    notification_QueryInterface,          notification_AddRef,         notification_Release,
    notification_OnDeviceStateChanged,    notification_OnDeviceAdded,  notification_OnDeviceRemoved,
    notification_OnDefaultDeviceChanged,  notification_OnPropertyValueChanged,
};

static IMMNotificationClient notificationClient = {&notificationVtbl};

void kinc_windows_co_initialize(void);

static bool initialized = false;

void kinc_a2_init() {
	if (initialized) {
		return;
	}

	kinc_a2_internal_init();
	kinc_mutex_init(&device_mutex);
	initialized = true;

	a2_buffer.read_location = 0;
	a2_buffer.write_location = 0;
	a2_buffer.data_size = 128 * 1024;
	a2_buffer.channel_count = 2;
	a2_buffer.channels[0] = (float *)malloc(a2_buffer.data_size * sizeof(float));
	a2_buffer.channels[1] = (float *)malloc(a2_buffer.data_size * sizeof(float));

	reinitEvent = CreateEvent(0, FALSE, FALSE, 0);
	kinc_affirm(reinitEvent != 0);

	kinc_windows_co_initialize();
	kinc_microsoft_affirm(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&deviceEnumerator));
	kinc_microsoft_affirm(deviceEnumerator->lpVtbl->RegisterEndpointNotificationCallback(deviceEnumerator, &notificationClient));

	// The first device opens here, before the audio thread exists - no kinc_a2 callback can be set yet.
	bool fell_back = false;
	bool selected = false;
	IMMDevice *first = findDevice(&fell_back, &selected);
	current_id = deviceId(first);
	initDevice(first);
	// The thread runs even without a device, to pick one up when it appears.
	CreateThread(0, 65536, audioThread, NULL, 0, 0);
}

void kinc_a2_update() {}

static void free_device_list(void) {
	for (int i = 0; i < device_list_count; ++i) {
		free(device_list[i].id);
	}
	free(device_list);
	device_list = NULL;
	device_list_count = 0;
}

int kinc_a2_device_count(void) {
	if (!initialized) {
		return 0;
	}

	IMMDeviceCollection *collection = NULL;
	UINT count = 0;
	if (deviceEnumerator->lpVtbl->EnumAudioEndpoints(deviceEnumerator, eRender, DEVICE_STATE_ACTIVE, &collection) != S_OK ||
	    collection->lpVtbl->GetCount(collection, &count) != S_OK) {
		count = 0;
	}

	output_device_t *list = count > 0 ? (output_device_t *)calloc(count, sizeof(output_device_t)) : NULL;
	int listed = 0;
	for (UINT i = 0; i < count && list != NULL; ++i) {
		IMMDevice *item = NULL;
		if (collection->lpVtbl->Item(collection, i, &item) != S_OK) {
			continue;
		}
		wchar_t *id = deviceId(item);
		if (id == NULL) {
			item->lpVtbl->Release(item);
			continue;
		}
		list[listed].id = id;
		list[listed].name[0] = 0;

		IPropertyStore *store = NULL;
		if (item->lpVtbl->OpenPropertyStore(item, STGM_READ, &store) == S_OK) {
			PROPVARIANT value;
			PropVariantInit(&value);
			if (store->lpVtbl->GetValue(store, &kinc_PKEY_Device_FriendlyName, &value) == S_OK && value.vt == VT_LPWSTR && value.pwszVal != NULL) {
				WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, list[listed].name, sizeof(list[listed].name), NULL, NULL);
				list[listed].name[sizeof(list[listed].name) - 1] = 0;
			}
			PropVariantClear(&value);
			store->lpVtbl->Release(store);
		}
		if (list[listed].name[0] == 0) {
			snprintf(list[listed].name, sizeof(list[listed].name), "Audio device %u", (unsigned int)listed);
		}
		item->lpVtbl->Release(item);
		++listed;
	}
	if (collection != NULL) {
		collection->lpVtbl->Release(collection);
	}

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
		free(selected_id);
		selected_id = index == KINC_A2_DEFAULT_DEVICE ? NULL : copy_wstring(device_list[index].id);
	}
	kinc_mutex_unlock(&device_mutex);
	if (valid) {
		requestReinit();
	}
	return valid;
}

int kinc_a2_selected_device(void) {
	if (!initialized) {
		return KINC_A2_DEFAULT_DEVICE;
	}
	int index = KINC_A2_DEFAULT_DEVICE;
	kinc_mutex_lock(&device_mutex);
	if (selected_id != NULL) {
		for (int i = 0; i < device_list_count; ++i) {
			if (wcscmp(device_list[i].id, selected_id) == 0) {
				index = i;
				break;
			}
		}
	}
	kinc_mutex_unlock(&device_mutex);
	return index;
}

#define SAFE_RELEASE(punk)                                                                                                                                     \
	if ((punk) != NULL) {                                                                                                                                      \
		(punk)->Release();                                                                                                                                     \
		(punk) = NULL;                                                                                                                                         \
	}

void kinc_a2_shutdown() {
	// Wait for last data in buffer to play before stopping.
	// Sleep((DWORD)(hnsActualDuration/REFTIMES_PER_MILLISEC/2));

	//	affirm(pAudioClient->Stop());  // Stop playing.

	//	CoTaskMemFree(pwfx);
	//	SAFE_RELEASE(pEnumerator)
	//	SAFE_RELEASE(pDevice)
	//	SAFE_RELEASE(pAudioClient)
	//	SAFE_RELEASE(pRenderClient)
}
