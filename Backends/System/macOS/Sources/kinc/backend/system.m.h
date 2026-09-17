#import "BasicOpenGLView.h"

#import <Cocoa/Cocoa.h>

#include <kinc/backend/HIDManager.h>
#include <kinc/graphics4/graphics.h>
#include <kinc/input/keyboard.h>
#include <kinc/log.h>
#include <kinc/system.h>
#include <kinc/window.h>

#include "windowdata.h"

#include <kinc/backend/windowdata.h>

bool withAutoreleasepool(bool (*f)(void)) {
	@autoreleasepool {
		return f();
	}
}

extern const char *macgetresourcepath(void);

const char *macgetresourcepath(void) {
	return [[[NSBundle mainBundle] resourcePath] cStringUsingEncoding:NSUTF8StringEncoding];
}

@interface KincApplication : NSApplication {
}
- (void)terminate:(id)sender;
@end

@interface KincAppDelegate : NSObject <NSWindowDelegate> {
}
- (void)windowWillClose:(NSNotification *)notification;
- (void)windowDidResize:(NSNotification *)notification;
- (void)windowWillMiniaturize:(NSNotification *)notification;
- (void)windowDidDeminiaturize:(NSNotification *)notification;
- (void)windowDidResignMain:(NSNotification *)notification;
- (void)windowDidBecomeMain:(NSNotification *)notification;
@end

static NSApplication *myapp;
static NSWindow *window;
static BasicOpenGLView *view;
static KincAppDelegate *delegate;
static struct HIDManager *hidManager;

/*struct KoreWindow : public KoreWindowBase {
    NSWindow* handle;
    BasicOpenGLView* view;

    KoreWindow(NSWindow* handle, BasicOpenGLView* view, int x, int y, int width, int height)
        : KoreWindowBase(x, y, width, height), handle(handle), view(view) {
        ::view = view;
    }
};*/

#ifdef KINC_METAL
CAMetalLayer *getMetalLayer(void) {
	return [view metalLayer];
}

id getMetalDevice(void) {
	return [view metalDevice];
}

id getMetalLibrary(void) {
	return [view metalLibrary];
}

id getMetalQueue(void) {
	return [view metalQueue];
}
#endif

bool kinc_internal_handle_messages(void) {
	NSEvent *event = [myapp nextEventMatchingMask:NSEventMaskAny
	                                    untilDate:[NSDate distantPast]
	                                       inMode:NSDefaultRunLoopMode
	                                      dequeue:YES]; // distantPast: non-blocking
	if (event != nil) {
		[myapp sendEvent:event];
		[myapp updateWindows];
	}

	// Sleep for a frame to limit the calls when the window is not visible.
	if (!window.visible) {
		[NSThread sleepForTimeInterval:1.0 / 60];
	}
	return true;
}

void swapBuffersMac(int windowId) {
#ifndef KINC_METAL
	[windows[windowId].view switchBuffers];
#endif
}

static int createWindow(kinc_window_options_t *options) {
	int width = options->width / [[NSScreen mainScreen] backingScaleFactor];
	int height = options->height / [[NSScreen mainScreen] backingScaleFactor];
	int styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
	if ((options->window_features & KINC_WINDOW_FEATURE_RESIZEABLE) || (options->window_features & KINC_WINDOW_FEATURE_MAXIMIZABLE)) {
		styleMask |= NSWindowStyleMaskResizable;
	}
	if (options->window_features & KINC_WINDOW_FEATURE_MINIMIZABLE) {
		styleMask |= NSWindowStyleMaskMiniaturizable;
	}

	view = [[BasicOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
	[view registerForDraggedTypes:[NSArray arrayWithObjects:NSURLPboardType, nil]];
	window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height) styleMask:styleMask backing:NSBackingStoreBuffered defer:TRUE];
	delegate = [KincAppDelegate alloc];
	[window setDelegate:delegate];
	[window setTitle:[NSString stringWithCString:options->title encoding:NSUTF8StringEncoding]];
	[window setAcceptsMouseMovedEvents:YES];
	[[window contentView] addSubview:view];
	[window center];

	windows[windowCounter].handle = window;
	windows[windowCounter].view = view;

	[window makeKeyAndOrderFront:nil];

	if (options->mode == KINC_WINDOW_MODE_FULLSCREEN || options->mode == KINC_WINDOW_MODE_EXCLUSIVE_FULLSCREEN) {
		[window toggleFullScreen:nil];
		windows[windowCounter].fullscreen = true;
	}

	return windowCounter++;
}

int kinc_count_windows(void) {
	return windowCounter;
}

// Zoom change waiting for an asynchronous fullscreen exit to finish:
// 0 = none, 1 = zoom, 2 = unzoom. Applied from windowDidExitFullScreen.
static int kinc_macos_pending_zoom[10] = {0};

static bool kinc_macos_window_is_fullscreen(int window_index) {
	// Ask the window rather than trusting the flag: the user can enter or leave
	// fullscreen through the title-bar button without Kinc seeing it.
	NSWindow *w = windows[window_index].handle;
	return w != nil && ([w styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

void kinc_window_change_window_mode(int window_index, kinc_window_mode_t mode) {
	NSWindow *w = windows[window_index].handle;
	if (w == nil) {
		return;
	}
	bool fullscreen = kinc_macos_window_is_fullscreen(window_index);
	switch (mode) {
	case KINC_WINDOW_MODE_WINDOW:
		if (fullscreen) {
			[w toggleFullScreen:nil];
			windows[window_index].fullscreen = false;
		}
		break;
	case KINC_WINDOW_MODE_FULLSCREEN:
	case KINC_WINDOW_MODE_EXCLUSIVE_FULLSCREEN:
		kinc_macos_pending_zoom[window_index] = 0;
		if (!fullscreen) {
			[w toggleFullScreen:nil];
			windows[window_index].fullscreen = true;
		}
		break;
	}
}

// The frame to go back to on unzoom. AppKit's own zoom: bookkeeping does not
// survive a fullscreen round trip (zoom: on a zoomed window that just left
// fullscreen "restores" to the zoomed frame), so remember it ourselves.
static NSRect kinc_macos_unzoomed_frame[10];
static bool kinc_macos_has_unzoomed_frame[10] = {false};

static void kinc_macos_apply_zoom(int window_index, bool maximized) {
	NSWindow *w = windows[window_index].handle;
	// zoom: toggles, so only send it when the state actually has to change.
	if ([w isZoomed] == maximized) {
		return;
	}
	if (maximized) {
		kinc_macos_unzoomed_frame[window_index] = [w frame];
		kinc_macos_has_unzoomed_frame[window_index] = true;
		[w zoom:nil];
	}
	else if (kinc_macos_has_unzoomed_frame[window_index]) {
		[w setFrame:kinc_macos_unzoomed_frame[window_index] display:YES animate:YES];
	}
	else {
		[w zoom:nil];
	}
}

void kinc_window_set_maximized(int window_index, bool maximized) {
	NSWindow *w = windows[window_index].handle;
	if (w == nil) {
		return;
	}
	if (kinc_macos_window_is_fullscreen(window_index)) {
		// toggleFullScreen: animates and only lands later; zooming now
		// reads the pre-exit frame (and unzoom silently no-ops).
		kinc_macos_pending_zoom[window_index] = maximized ? 1 : 2;
		kinc_window_change_window_mode(window_index, KINC_WINDOW_MODE_WINDOW);
		return;
	}
	kinc_macos_pending_zoom[window_index] = 0;
	kinc_macos_apply_zoom(window_index, maximized);
}

void kinc_window_set_close_callback(int window, bool (*callback)(void *), void *data) {
	windows[window].closeCallback = callback;
	windows[window].closeCallbackData = data;
}

static void addMenubar(void) {
	NSString *appName = [[NSProcessInfo processInfo] processName];

	NSMenu *appMenu = [NSMenu new];
	NSString *quitTitle = [@"Quit " stringByAppendingString:appName];
	NSMenuItem *quitMenuItem = [[NSMenuItem alloc] initWithTitle:quitTitle action:@selector(terminate:) keyEquivalent:@"q"];
	[appMenu addItem:quitMenuItem];

	NSMenuItem *appMenuItem = [NSMenuItem new];
	[appMenuItem setSubmenu:appMenu];

	NSMenu *menubar = [NSMenu new];
	[menubar addItem:appMenuItem];
	[NSApp setMainMenu:menubar];
}

int kinc_init(const char *name, int width, int height, kinc_window_options_t *win, kinc_framebuffer_options_t *frame) {
	@autoreleasepool {
		myapp = [KincApplication sharedApplication];
		[myapp finishLaunching];
		[[NSRunningApplication currentApplication] activateWithOptions:(NSApplicationActivateAllWindows | NSApplicationActivateIgnoringOtherApps)];
		NSApp.activationPolicy = NSApplicationActivationPolicyRegular;

		hidManager = (struct HIDManager *)malloc(sizeof(struct HIDManager));
		HIDManager_init(hidManager);
		addMenubar();
	}

	// System::_init(name, width, height, &win, &frame);
	kinc_window_options_t defaultWindowOptions;
	if (win == NULL) {
		kinc_window_options_set_defaults(&defaultWindowOptions);
		win = &defaultWindowOptions;
	}

	kinc_framebuffer_options_t defaultFramebufferOptions;
	if (frame == NULL) {
		kinc_framebuffer_options_set_defaults(&defaultFramebufferOptions);
		frame = &defaultFramebufferOptions;
	}

	win->width = width;
	win->height = height;
	if (win->title == NULL) {
		win->title = name;
	}

	int windowId = createWindow(win);
	kinc_g4_internal_init();
	kinc_g4_internal_init_window(windowId, frame->depth_bits, frame->stencil_bits, true);

	return 0;
}

int kinc_window_width(int window_index) {
	NSWindow *window = windows[window_index].handle;
	float scale = [window backingScaleFactor];
	return [[window contentView] frame].size.width * scale;
}

int kinc_window_height(int window_index) {
	NSWindow *window = windows[window_index].handle;
	float scale = [window backingScaleFactor];
	return [[window contentView] frame].size.height * scale;
}

NSWindow *kinc_get_mac_window_handle(int window_index) {
	return windows[window_index].handle;
}

void kinc_load_url(const char *url) {
	[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url]]];
}

static char language[3];

const char *kinc_language(void) {
	NSString *nsstr = [[NSLocale preferredLanguages] objectAtIndex:0];
	const char *lang = [nsstr UTF8String];
	language[0] = lang[0];
	language[1] = lang[1];
	language[2] = 0;
	return language;
}

void kinc_internal_shutdown(void) {}

static const char *getSavePath(void) {
	NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
	NSString *resolvedPath = [paths objectAtIndex:0];
	NSString *appName = [NSString stringWithUTF8String:kinc_application_name()];
	resolvedPath = [resolvedPath stringByAppendingPathComponent:appName];

	NSFileManager *fileMgr = [[NSFileManager alloc] init];

	NSError *error;
	[fileMgr createDirectoryAtPath:resolvedPath withIntermediateDirectories:YES attributes:nil error:&error];

	resolvedPath = [resolvedPath stringByAppendingString:@"/"];
	return [resolvedPath cStringUsingEncoding:NSUTF8StringEncoding];
}

const char *kinc_internal_save_path(void) {
	return getSavePath();
}

#ifndef KINC_NO_MAIN
int main(int argc, char **argv) {
	return kickstart(argc, argv);
}
#endif

@implementation KincApplication

- (void)terminate:(id)sender {
	kinc_stop();
}

@end

@implementation KincAppDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
	if (windows[0].closeCallback != NULL) {
		if (windows[0].closeCallback(windows[0].closeCallbackData)) {
			return YES;
		}
		else {
			return NO;
		}
	}
	return YES;
}

- (void)windowWillClose:(NSNotification *)notification {
	kinc_stop();
}

- (void)windowDidResize:(NSNotification *)notification {
	NSWindow *window = [notification object];
	NSSize size = [[window contentView] frame].size;
	[view resize:size];
	if (windows[0].resizeCallback != NULL) {
		windows[0].resizeCallback(size.width, size.height, windows[0].resizeCallbackData);
	}
}

- (void)windowDidExitFullScreen:(NSNotification *)notification {
	NSWindow *window = [notification object];
	for (int i = 0; i < windowCounter; ++i) {
		if (windows[i].handle == window && kinc_macos_pending_zoom[i] != 0) {
			bool maximized = kinc_macos_pending_zoom[i] == 1;
			kinc_macos_pending_zoom[i] = 0;
			int idx = i;
			// Let the exit transition fully settle before touching the frame.
			dispatch_async(dispatch_get_main_queue(), ^{
				kinc_macos_apply_zoom(idx, maximized);
			});
		}
	}
}

- (void)windowWillMiniaturize:(NSNotification *)notification {
	kinc_internal_background_callback();
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
	kinc_internal_foreground_callback();
}

- (void)windowDidResignMain:(NSNotification *)notification {
	kinc_internal_pause_callback();
}

- (void)windowDidBecomeMain:(NSNotification *)notification {
	kinc_internal_resume_callback();
}

@end
