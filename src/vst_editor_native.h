#pragma once
// CAW - the native window that holds a VST3 plugin's own editor on macOS (Cocoa) and Linux (X11).
// Windows uses its own HWND code in vst3_host.cpp. Kept in separate files so the X11 / Cocoa
// headers never mix with the VST3 SDK headers (X11 defines macros like None / Bool / Status).
namespace cawed {
struct EdWin;                                                     // opaque native window
EdWin* create(const char* title, int width, int height, bool resizable);
void*  nativeHandle(EdWin* w);                                    // X11: the Window id as void*; macOS: the NSView*
void   setTitle(EdWin* w, const char* title);
void   resize(EdWin* w, int width, int height, bool resizable);   // the plugin asked for a new size
void   show(EdWin* w);
bool   visible(EdWin* w);
// Window events since the last call. Bit 1 = the user closed the window, bit 2 = the user
// resized it (new size in *width / *height). Called from the UI thread every frame.
int    poll(EdWin* w, int* width, int* height);
void   destroy(EdWin* w);
}
