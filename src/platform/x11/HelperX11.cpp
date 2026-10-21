// The window parts of platform/Helper.h on Linux: X11.
//
// A Linux VST2 editor gets an X11 window id as its parent. Window ids are global to the X
// server, so the bridge can create the editor's container as a child of a window that belongs
// to the plugin's (client's) process, just like a cross-process child HWND on Windows.
#include "platform/Helper.h"

#include "common/Util.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

namespace yov::helper {

namespace {

Display* gDpy = nullptr;  // our own connection: container / probe window (main thread)
Window gProbeWindow = 0;
void (*gReport)(const std::string&) = nullptr;
std::atomic<int> gXErrors{0};

// Plugins share the process-wide Xlib error handler. The default one exits the process on any
// error (a plugin's stray BadWindow would kill the bridge); count and carry on instead.
int onXError(Display*, XErrorEvent*) {
  ++gXErrors;
  return 0;
}

Display* dpy() {
  if (!gDpy) gDpy = XOpenDisplay(nullptr);
  return gDpy;
}

std::string windowTitle(Display* d, Window w) {
  Atom netName = XInternAtom(d, "_NET_WM_NAME", False);
  Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
  Atom type;
  int format;
  unsigned long n, after;
  unsigned char* data = nullptr;
  std::string t;
  if (XGetWindowProperty(d, w, netName, 0, 1024, False, utf8, &type, &format, &n, &after, &data) == Success && data) {
    t.assign((char*)data, n);
    XFree(data);
  }
  if (t.empty()) {
    char* name = nullptr;
    if (XFetchName(d, w, &name) && name) {
      t = name;
      XFree(name);
    }
  }
  return t;
}

long windowPid(Display* d, Window w) {
  Atom pidAtom = XInternAtom(d, "_NET_WM_PID", True);
  if (pidAtom == None) return -1;
  Atom type;
  int format;
  unsigned long n, after;
  unsigned char* data = nullptr;
  long pid = -1;
  if (XGetWindowProperty(d, w, pidAtom, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after, &data) == Success &&
      data) {
    if (n == 1 && format == 32) pid = (long)*(unsigned long*)data;
    XFree(data);
  }
  return pid;
}

// Top-level windows of this process: children of the root, or the client window inside a
// window manager's frame.
void collect(Display* d, Window w, int depth, long pid, std::vector<Window>& out) {
  Window root, parent, *kids = nullptr;
  unsigned int n = 0;
  if (!XQueryTree(d, w, &root, &parent, &kids, &n)) return;
  for (unsigned int i = 0; i < n; ++i) {
    Window k = kids[i];
    if (k == gProbeWindow) continue;
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, k, &a) || a.map_state != IsViewable) continue;
    if (windowPid(d, k) == pid) out.push_back(k);
    else if (depth < 2) collect(d, k, depth + 1, pid, out);
  }
  if (kids) XFree(kids);
}

void popupEater() {
  Display* d = XOpenDisplay(nullptr);
  if (!d) return;
  Atom protocols = XInternAtom(d, "WM_PROTOCOLS", False);
  Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
  std::map<Window, int> attempts;
  const long pid = (long)getpid();
  for (;;) {
    usleep(250000);
    std::vector<Window> found;
    collect(d, DefaultRootWindow(d), 0, pid, found);
    for (Window w : found) {
      int& n = attempts[w];
      if (n == 0) {
        std::string desc = windowTitle(d, w);
        if (desc.empty()) desc = "(untitled window)";
        if (desc.size() > 300) desc = desc.substr(0, 300) + "...";
        if (gReport) gReport(desc);
      }
      // Escalate: ask it to close (twice), then hide it.
      if (n < 2) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)del;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(d, w, False, NoEventMask, &ev);
      } else if (n == 2) {
        XUnmapWindow(d, w);
      }
      ++n;
    }
    XFlush(d);
  }
}

}  // namespace

void init(bool) {
  XInitThreads();  // before any other Xlib call: plugins may use Xlib from several threads
  XSetErrorHandler(onXError);
  signal(SIGPIPE, SIG_IGN);
}

void pumpMessages(int budget) {
  if (!gDpy) return;
  while (budget-- > 0 && XPending(gDpy)) {
    XEvent ev;
    XNextEvent(gDpy, &ev);
  }
}

bool haveDisplay() {
  const char* d = getenv("DISPLAY");
  return d && *d;
}

void* containerCreate(int64_t parent, int w, int h) {
  Display* d = dpy();
  if (!d || !parent) return nullptr;
  int errors = gXErrors.load();
  XSetWindowAttributes a{};
  a.background_pixel = BlackPixel(d, DefaultScreen(d));
  a.event_mask = StructureNotifyMask;
  Window win = XCreateWindow(d, (Window)parent, 0, 0, (unsigned)std::max(w, 1), (unsigned)std::max(h, 1), 0,
                             CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &a);
  XMapWindow(d, win);
  XSync(d, False);
  if (gXErrors.load() != errors) {  // the parent window is gone
    XDestroyWindow(d, win);
    XSync(d, False);
    return nullptr;
  }
  return (void*)(uintptr_t)win;
}

void containerResize(void* c, int w, int h) {
  if (!c || !gDpy) return;
  XResizeWindow(gDpy, (Window)(uintptr_t)c, (unsigned)std::max(w, 1), (unsigned)std::max(h, 1));
  XFlush(gDpy);
}

bool containerChildSize(void* c, int& w, int& h) {
  if (!c || !gDpy) return false;
  Window root, parent, *kids = nullptr;
  unsigned int n = 0;
  bool ok = false;
  if (XQueryTree(gDpy, (Window)(uintptr_t)c, &root, &parent, &kids, &n) && n > 0) {
    XWindowAttributes a;
    if (XGetWindowAttributes(gDpy, kids[0], &a)) {
      w = a.width;
      h = a.height;
      ok = true;
    }
  }
  if (kids) XFree(kids);
  return ok;
}

void containerDestroy(void* c) {
  if (!c || !gDpy) return;
  XDestroyWindow(gDpy, (Window)(uintptr_t)c);
  XSync(gDpy, False);
}

void* probeWindowCreate() {
  Display* d = dpy();
  if (!d) return nullptr;
  // Real hosts show the parent window; some editors fail with an unmapped one. Map it far
  // off-screen; override-redirect keeps the window manager from moving it into view.
  XSetWindowAttributes a{};
  a.override_redirect = True;
  a.background_pixel = BlackPixel(d, DefaultScreen(d));
  Window w = XCreateWindow(d, DefaultRootWindow(d), -3000, -3000, 1600, 1200, 0, CopyFromParent, InputOutput,
                           CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
  XStoreName(d, w, "probe");
  XMapWindow(d, w);
  XSync(d, False);
  gProbeWindow = w;
  return (void*)(uintptr_t)w;
}

void probeWindowDestroy(void* w) {
  if (!w || !gDpy) return;
  XDestroyWindow(gDpy, (Window)(uintptr_t)w);
  XSync(gDpy, False);
}

void startPopupEater(void (*report)(const std::string&)) {
  gReport = report;
  if (!haveDisplay()) return;
  std::thread(popupEater).detach();  // the process ends via exitNow, no join needed
}

}  // namespace yov::helper
