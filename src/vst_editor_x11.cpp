// CAW - VST3 editor window on Linux: a top-level X11 window the plugin embeds its view into
// (kPlatformTypeX11EmbedWindowID). Each window has its own Display connection, so its events
// are read without touching GLFW's connection or another plugin's window.
#if defined(__linux__) && defined(CAW_VST3)
#include "vst_editor_native.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cstdint>
#include <cstring>

namespace cawed {
struct EdWin {
    Display* dpy = nullptr;
    ::Window win = 0;
    Atom wmDelete = 0;
    bool mapped = false;
    int w = 0, h = 0;
};

static void sizeHints(EdWin* e, int w, int h, bool resizable) {
    XSizeHints* hints = XAllocSizeHints();
    if (!hints) return;
    hints->flags = PSize;
    hints->width = w; hints->height = h;
    if (!resizable) {   // a fixed-size editor: the window manager must not let the user drag it
        hints->flags |= PMinSize | PMaxSize;
        hints->min_width = hints->max_width = w;
        hints->min_height = hints->max_height = h;
    }
    XSetWMNormalHints(e->dpy, e->win, hints);
    XFree(hints);
}

EdWin* create(const char* title, int width, int height, bool resizable) {
    if (width <= 0) width = 400;
    if (height <= 0) height = 300;
    Display* d = XOpenDisplay(nullptr);
    if (!d) return nullptr;
    const int s = DefaultScreen(d);
    EdWin* e = new EdWin;
    e->dpy = d; e->w = width; e->h = height;
    e->win = XCreateSimpleWindow(d, RootWindow(d, s), 100, 100, (unsigned)width, (unsigned)height, 0,
                                 BlackPixel(d, s), BlackPixel(d, s));
    XSelectInput(d, e->win, StructureNotifyMask);
    e->wmDelete = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, e->win, &e->wmDelete, 1);
    XClassHint cls; char name[] = "caw-plugin"; char klass[] = "CAW";
    cls.res_name = name; cls.res_class = klass;
    XSetClassHint(d, e->win, &cls);
    sizeHints(e, width, height, resizable);
    setTitle(e, title);
    XMapWindow(d, e->win);
    XSync(d, False);
    e->mapped = true;
    return e;
}

void* nativeHandle(EdWin* e) { return e ? reinterpret_cast<void*>(static_cast<uintptr_t>(e->win)) : nullptr; }

void setTitle(EdWin* e, const char* title) {
    if (!e) return;
    const char* t = title ? title : "";
    XStoreName(e->dpy, e->win, t);
    const Atom netName = XInternAtom(e->dpy, "_NET_WM_NAME", False);
    const Atom utf8 = XInternAtom(e->dpy, "UTF8_STRING", False);
    XChangeProperty(e->dpy, e->win, netName, utf8, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(t), (int)std::strlen(t));
    XFlush(e->dpy);
}

void resize(EdWin* e, int width, int height, bool resizable) {
    if (!e || width <= 0 || height <= 0) return;
    e->w = width; e->h = height;   // set first: the ConfigureNotify this causes is not a user resize
    sizeHints(e, width, height, resizable);
    XResizeWindow(e->dpy, e->win, (unsigned)width, (unsigned)height);
    XFlush(e->dpy);
}

void show(EdWin* e) {
    if (!e) return;
    XMapRaised(e->dpy, e->win);
    XFlush(e->dpy);
    e->mapped = true;
}

bool visible(EdWin* e) { return e && e->mapped; }

int poll(EdWin* e, int* width, int* height) {
    if (!e) return 0;
    int flags = 0;
    while (XPending(e->dpy) > 0) {
        XEvent ev;
        XNextEvent(e->dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == e->wmDelete) flags |= 1;
        else if (ev.type == ConfigureNotify && ev.xconfigure.window == e->win) {
            if (ev.xconfigure.width != e->w || ev.xconfigure.height != e->h) {
                e->w = ev.xconfigure.width; e->h = ev.xconfigure.height; flags |= 2;
            }
        }
        else if (ev.type == UnmapNotify && ev.xunmap.window == e->win) e->mapped = false;
        else if (ev.type == MapNotify && ev.xmap.window == e->win) e->mapped = true;
    }
    if ((flags & 2) && width && height) { *width = e->w; *height = e->h; }
    return flags;
}

void destroy(EdWin* e) {
    if (!e) return;
    XDestroyWindow(e->dpy, e->win);
    XSync(e->dpy, False);
    XCloseDisplay(e->dpy);
    delete e;
}
}
#endif
