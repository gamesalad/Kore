#pragma once

#include <Android/android_native_app_glue.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// name in usual Java syntax (points, no slashes)
jclass kinc_android_find_class(JNIEnv *env, const char *name);

ANativeActivity *kinc_android_get_activity(void);

AAssetManager *kinc_android_get_asset_manager(void);

// Optional application hook consulted for every key event BEFORE Kinc's own
// keycode → keyboard/gamepad mapping. Return true to claim the event (Kinc
// then does nothing with it), false to let the default mapping run. Lets an
// application remap TV-remote / media keys without editing this backend.
typedef bool (*kinc_android_key_hook_t)(int keycode, bool down, void *userdata);
void kinc_android_set_key_hook(kinc_android_key_hook_t hook, void *userdata);

#ifdef __cplusplus
}
#endif
