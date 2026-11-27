// CAW - VST3 editor window on macOS: an NSWindow whose content view the plugin attaches to
// (kPlatformTypeNSView). Compiled with ARC (see CMakeLists.txt). All calls come from the main
// thread (CAW's UI); GLFW's event loop drives Cocoa, so the plugin's own timers run as usual.
#if defined(__APPLE__) && defined(CAW_VST3)
#import <Cocoa/Cocoa.h>
#include "vst_editor_native.h"

@interface CawEditorDelegate : NSObject <NSWindowDelegate> {
@public
    int flags;
    int w, h;
}
@end

@implementation CawEditorDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    flags |= 1;                 // CAW tears the view down on its next frame (outside the plugin's code)
    [sender orderOut:nil];
    return NO;
}
- (void)windowDidResize:(NSNotification*)note {
    NSWindow* win = (NSWindow*)note.object;
    const NSRect r = [win contentRectForFrameRect:win.frame];
    const int nw = (int)r.size.width, nh = (int)r.size.height;
    if (nw != w || nh != h) { w = nw; h = nh; flags |= 2; }
}
@end

namespace cawed {
struct EdWin {
    NSWindow* win = nil;
    CawEditorDelegate* del = nil;
};

static NSWindowStyleMask styleFor(bool resizable) {
    NSWindowStyleMask m = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (resizable) m |= NSWindowStyleMaskResizable;
    return m;
}

EdWin* create(const char* title, int width, int height, bool resizable) {
    @autoreleasepool {
        if (width <= 0) width = 400;
        if (height <= 0) height = 300;
        NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, width, height)
                                                    styleMask:styleFor(resizable)
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        if (!win) return nullptr;
        win.releasedWhenClosed = NO;
        CawEditorDelegate* del = [[CawEditorDelegate alloc] init];
        del->flags = 0; del->w = width; del->h = height;
        win.delegate = del;
        [win center];
        EdWin* e = new EdWin;
        e->win = win; e->del = del;
        setTitle(e, title);
        return e;
    }
}

void* nativeHandle(EdWin* e) { return e ? (__bridge void*)[e->win contentView] : nullptr; }

void setTitle(EdWin* e, const char* title) {
    if (!e) return;
    [e->win setTitle:[NSString stringWithUTF8String:(title ? title : "")]];
}

void resize(EdWin* e, int width, int height, bool resizable) {
    if (!e || width <= 0 || height <= 0) return;
    e->del->w = width; e->del->h = height;   // set first: the resize notification is not a user resize
    [e->win setStyleMask:styleFor(resizable)];
    [e->win setContentSize:NSMakeSize(width, height)];
}

void show(EdWin* e) {
    if (!e) return;
    [e->win makeKeyAndOrderFront:nil];
}

bool visible(EdWin* e) { return e && [e->win isVisible]; }

int poll(EdWin* e, int* width, int* height) {
    if (!e) return 0;
    const int f = e->del->flags;
    e->del->flags = 0;
    if ((f & 2) && width && height) { *width = e->del->w; *height = e->del->h; }
    return f;
}

void destroy(EdWin* e) {
    if (!e) return;
    e->win.delegate = nil;
    [e->win orderOut:nil];
    [e->win close];
    e->win = nil; e->del = nil;   // ARC releases them
    delete e;
}
}
#endif
