// GuiWindow on Linux: an X11 child window of the host's window with a GLX context, and an X11
// child window for the plugin's editor (a VST2 editor on Linux gets an X11 window id as parent).
//
// X11 has no per-thread message queue a host would pump for us: the host calls us through the
// CLAP timer-support (a periodic tick) and posix-fd-support (our X connection has input)
// extensions, always on its main thread.
#include "plugin/imgui/GuiWindow.h"

#include "common/Util.h"
#include "platform/Process.h"
#include "plugin/Plugin.h"

#include <imgui.h>

#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace yov {

namespace {

ImGuiKey mapKey(KeySym k) {
  if (k >= XK_a && k <= XK_z) return (ImGuiKey)(ImGuiKey_A + (k - XK_a));
  if (k >= XK_A && k <= XK_Z) return (ImGuiKey)(ImGuiKey_A + (k - XK_A));
  if (k >= XK_0 && k <= XK_9) return (ImGuiKey)(ImGuiKey_0 + (k - XK_0));
  if (k >= XK_F1 && k <= XK_F12) return (ImGuiKey)(ImGuiKey_F1 + (k - XK_F1));
  switch (k) {
    case XK_Tab:
    case XK_ISO_Left_Tab: return ImGuiKey_Tab;
    case XK_Left: return ImGuiKey_LeftArrow;
    case XK_Right: return ImGuiKey_RightArrow;
    case XK_Up: return ImGuiKey_UpArrow;
    case XK_Down: return ImGuiKey_DownArrow;
    case XK_Page_Up: return ImGuiKey_PageUp;
    case XK_Page_Down: return ImGuiKey_PageDown;
    case XK_Home: return ImGuiKey_Home;
    case XK_End: return ImGuiKey_End;
    case XK_Insert: return ImGuiKey_Insert;
    case XK_Delete: return ImGuiKey_Delete;
    case XK_BackSpace: return ImGuiKey_Backspace;
    case XK_space: return ImGuiKey_Space;
    case XK_Return: return ImGuiKey_Enter;
    case XK_KP_Enter: return ImGuiKey_KeypadEnter;
    case XK_Escape: return ImGuiKey_Escape;
    case XK_Shift_L: return ImGuiKey_LeftShift;
    case XK_Shift_R: return ImGuiKey_RightShift;
    case XK_Control_L: return ImGuiKey_LeftCtrl;
    case XK_Control_R: return ImGuiKey_RightCtrl;
    case XK_Alt_L: return ImGuiKey_LeftAlt;
    case XK_Alt_R: return ImGuiKey_RightAlt;
    case XK_Super_L: return ImGuiKey_LeftSuper;
    case XK_Super_R: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
  }
}

// X errors on our own connections (a parent window the host destroyed first, ...) are ours to
// ignore; Xlib's default handler would exit the host. Errors on every other connection go to the
// handler that was installed before ours. The handler is process-wide, so it is installed once
// and only put back when the plugin binary is unloaded.
std::mutex gErrM;
std::vector<Display*> gOurDisplays;
XErrorHandler gPrevHandler = nullptr;
bool gHandlerInstalled = false;
std::atomic<bool> gXError{false};

int onXError(Display* d, XErrorEvent* e) {
  {
    std::lock_guard<std::mutex> lk(gErrM);
    if (std::find(gOurDisplays.begin(), gOurDisplays.end(), d) != gOurDisplays.end()) {
      gXError = true;
      return 0;
    }
  }
  return gPrevHandler ? gPrevHandler(d, e) : 0;
}

void adoptDisplay(Display* d) {
  std::lock_guard<std::mutex> lk(gErrM);
  gOurDisplays.push_back(d);
  if (!gHandlerInstalled) {
    gHandlerInstalled = true;
    gPrevHandler = XSetErrorHandler(onXError);
  }
}

void releaseDisplay(Display* d) {
  std::lock_guard<std::mutex> lk(gErrM);
  gOurDisplays.erase(std::remove(gOurDisplays.begin(), gOurDisplays.end(), d), gOurDisplays.end());
}

class X11GuiWindow final : public GuiWindow {
 public:
  explicit X11GuiWindow(Plugin& p) : plugin_(p) {}
  ~X11GuiWindow() override;

  bool attach(const clap_window_t* parent, int w, int h) override;
  void resize(int w, int h) override;
  void setVisible(bool visible) override;
  float scale() const override { return scale_; }
  void startTicking(std::function<void()> tick) override;
  void onHostTimer(clap_id id) override {
    if (id == timer_ && tick_) tick_();
  }
  void onHostFd(int fd, clap_posix_fd_flags_t) override {
    if (dpy_ && fd == ConnectionNumber(dpy_) && tick_) tick_();
  }
  bool pollEvents() override;
  bool beginGl(int width, int height, uint32_t clearRgb) override;
  void endGl(bool swap) override;
  void* editorParent() override { return (void*)(uintptr_t)editor_; }
  void placeEditor(int x, int y, int w, int h, bool visible) override;
  bool editorChildSize(int& w, int& h) override;
  void destroyEditorLeftovers() override {}
  void revealInFileManager(const std::string& path) override;

 private:
  void setCursor(ImGuiMouseCursor c);
  void serveSelection(const XSelectionRequestEvent& req);
  static void setClipboard(ImGuiContext*, const char* text);
  static const char* getClipboard(ImGuiContext*);

  Plugin& plugin_;
  Display* dpy_ = nullptr;
  Window parent_ = 0, win_ = 0, editor_ = 0;
  Colormap cmap_ = 0;
  GLXContext ctx_ = nullptr;
  XIM xim_ = nullptr;
  XIC xic_ = nullptr;
  Atom atomClipboard_ = 0, atomUtf8_ = 0, atomTargets_ = 0, atomProp_ = 0;
  std::string clipOwned_, clipGot_;
  float scale_ = 1.f;
  clap_id timer_ = CLAP_INVALID_ID;
  bool fdRegistered_ = false;
  std::function<void()> tick_;
  Cursor cursors_[ImGuiMouseCursor_COUNT] = {};
  int lastCursor_ = -100;
  bool swapIntervalSet_ = false;
  bool gone_ = false;  // the host destroyed our window (with its own) before destroying us
  bool visible_ = true;
  bool editorMapped_ = false;
  int editorX_ = 0, editorY_ = 0, editorW_ = 0, editorH_ = 0;
  // GL state of the host, put back after we draw
  Display* prevDpy_ = nullptr;
  GLXDrawable prevDraw_ = 0, prevRead_ = 0;
  GLXContext prevCtx_ = nullptr;

};

X11GuiWindow::~X11GuiWindow() {
  if (timer_ != CLAP_INVALID_ID && plugin_.hostTimer()) plugin_.hostTimer()->unregister_timer(plugin_.host(), timer_);
  if (fdRegistered_ && plugin_.hostFd()) plugin_.hostFd()->unregister_fd(plugin_.host(), ConnectionNumber(dpy_));
  if (!dpy_) return;
  if (ctx_) {
    if (glXGetCurrentContext() == ctx_) glXMakeCurrent(dpy_, None, nullptr);
    glXDestroyContext(dpy_, ctx_);
  }
  if (xic_) XDestroyIC(xic_);
  if (xim_) XCloseIM(xim_);
  for (Cursor c : cursors_)
    if (c) XFreeCursor(dpy_, c);
  if (win_ && !gone_) XDestroyWindow(dpy_, win_);
  if (cmap_) XFreeColormap(dpy_, cmap_);
  XSync(dpy_, False);
  releaseDisplay(dpy_);
  XCloseDisplay(dpy_);
}

bool X11GuiWindow::attach(const clap_window_t* parent, int w, int h) {
  if (win_) {
    // Re-parented by the host.
    parent_ = (Window)parent->x11;
    XReparentWindow(dpy_, win_, parent_, 0, 0);
    XFlush(dpy_);
    return true;
  }
  dpy_ = XOpenDisplay(nullptr);
  if (!dpy_) {
    logLine("gui: cannot open the X display");
    return false;
  }
  adoptDisplay(dpy_);
  parent_ = (Window)parent->x11;
  int screen = DefaultScreen(dpy_);
  int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
  XVisualInfo* vi = glXChooseVisual(dpy_, screen, attrs);
  if (!vi) {
    logLine("gui: no OpenGL visual");
    return false;
  }
  cmap_ = XCreateColormap(dpy_, RootWindow(dpy_, screen), vi->visual, AllocNone);
  XSetWindowAttributes swa{};
  swa.colormap = cmap_;
  swa.border_pixel = 0;
  swa.background_pixel = 0x1C1D21;
  swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                   ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask;
  XSync(dpy_, False);
  gXError = false;
  win_ = XCreateWindow(dpy_, parent_, 0, 0, (unsigned)std::max(w, 1), (unsigned)std::max(h, 1), 0, vi->depth,
                       InputOutput, vi->visual, CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);
  XSetWindowAttributes ea{};
  ea.background_pixel = 0x1C1D21;
  ea.event_mask = StructureNotifyMask;
  editor_ = XCreateWindow(dpy_, win_, 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
                          CWBackPixel | CWEventMask, &ea);
  XMapWindow(dpy_, win_);
  XSync(dpy_, False);
  if (gXError) {
    logLine("gui: cannot create the window in the host's window");
    XFree(vi);
    return false;
  }
  ctx_ = glXCreateContext(dpy_, vi, nullptr, True);
  XFree(vi);
  if (!ctx_) {
    logLine("gui: cannot create an OpenGL context");
    return false;
  }

  // Text input (UTF-8 through the input method when there is one).
  XSetLocaleModifiers("");
  xim_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
  if (xim_)
    xic_ = XCreateIC(xim_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win_, XNFocusWindow,
                     win_, nullptr);

  // Scale: the desktop's Xft.dpi.
  if (const char* rms = XResourceManagerString(dpy_)) {
    XrmInitialize();
    XrmDatabase db = XrmGetStringDatabase(rms);
    char* type = nullptr;
    XrmValue v{};
    if (db && XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &v) && v.addr) {
      double dpi = atof(v.addr);
      if (dpi >= 48 && dpi <= 480) scale_ = (float)(dpi / 96.0);
    }
    if (db) XrmDestroyDatabase(db);
  }
  std::string over;
  if (getEnv("YEOLDEVST_SCALE", over) && atof(over.c_str()) > 0.4) scale_ = (float)atof(over.c_str());

  atomClipboard_ = XInternAtom(dpy_, "CLIPBOARD", False);
  atomUtf8_ = XInternAtom(dpy_, "UTF8_STRING", False);
  atomTargets_ = XInternAtom(dpy_, "TARGETS", False);
  atomProp_ = XInternAtom(dpy_, "YEOLDEVST_CLIP", False);

  const unsigned shapes[ImGuiMouseCursor_COUNT] = {XC_left_ptr,  XC_xterm,           XC_fleur,
                                                   XC_sb_v_double_arrow, XC_sb_h_double_arrow, XC_bottom_left_corner,
                                                   XC_bottom_right_corner, XC_hand2, XC_watch, XC_watch, XC_X_cursor};
  for (int i = 0; i < ImGuiMouseCursor_COUNT; ++i) cursors_[i] = XCreateFontCursor(dpy_, shapes[i]);

  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  pio.Platform_SetClipboardTextFn = setClipboard;
  pio.Platform_GetClipboardTextFn = getClipboard;
  pio.Platform_ClipboardUserData = this;
  return true;
}

void X11GuiWindow::resize(int w, int h) {
  if (!win_ || gone_) return;
  XResizeWindow(dpy_, win_, (unsigned)std::max(w, 1), (unsigned)std::max(h, 1));
  XFlush(dpy_);
}

void X11GuiWindow::setVisible(bool visible) {
  if (!win_ || gone_ || visible == visible_) return;
  visible_ = visible;
  if (visible) XMapWindow(dpy_, win_);
  else XUnmapWindow(dpy_, win_);
  XFlush(dpy_);
}

void X11GuiWindow::startTicking(std::function<void()> tick) {
  tick_ = std::move(tick);
  if (auto t = plugin_.hostTimer()) {
    if (!t->register_timer(plugin_.host(), 15, &timer_)) timer_ = CLAP_INVALID_ID;
  }
  if (auto f = plugin_.hostFd()) fdRegistered_ = f->register_fd(plugin_.host(), ConnectionNumber(dpy_), CLAP_POSIX_FD_READ);
  if (timer_ == CLAP_INVALID_ID)
    logLine("gui: the host offers no timer (clap.timer-support); the window will not update");
}

bool X11GuiWindow::pollEvents() {
  if (!dpy_) return false;
  ImGuiIO& io = ImGui::GetIO();
  bool any = false;
  static const bool debug = [] {
    std::string v;
    return getEnv("YEOLDEVST_GUI_DEBUG", v);
  }();
  while (XPending(dpy_)) {
    XEvent ev;
    XNextEvent(dpy_, &ev);
    if (debug) fprintf(stderr, "x11 event %d window 0x%lx (ours 0x%lx)\n", ev.type, ev.xany.window, win_);
    if (XFilterEvent(&ev, None)) {
      if (debug) fprintf(stderr, "  filtered by the input method\n");
      continue;
    }
    if (ev.type == MappingNotify) {
      // The keyboard layout changed (or a tool remapped a spare key code to type with).
      XRefreshKeyboardMapping(&ev.xmapping);
      continue;
    }
    if (ev.xany.window != win_ && ev.type != SelectionRequest && ev.type != SelectionNotify &&
        ev.type != SelectionClear)
      continue;
    switch (ev.type) {
      case Expose: any = true; break;
      case DestroyNotify:
        if (ev.xdestroywindow.window == win_) gone_ = true;
        break;
      case MotionNotify:
        io.AddMousePosEvent((float)ev.xmotion.x, (float)ev.xmotion.y);
        any = true;
        break;
      case LeaveNotify:
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        any = true;
        break;
      case EnterNotify:
        io.AddMousePosEvent((float)ev.xcrossing.x, (float)ev.xcrossing.y);
        any = true;
        break;
      case ButtonPress:
      case ButtonRelease: {
        bool down = ev.type == ButtonPress;
        unsigned b = ev.xbutton.button;
        io.AddMousePosEvent((float)ev.xbutton.x, (float)ev.xbutton.y);
        if (b == 1 || b == 2 || b == 3) {
          io.AddMouseButtonEvent(b == 1 ? 0 : b == 3 ? 1 : 2, down);
          if (down) {
            // Keys go to the focused window: take focus when clicked, like any embedded editor.
            Window focus;
            int revert;
            XGetInputFocus(dpy_, &focus, &revert);
            if (focus != win_) XSetInputFocus(dpy_, win_, RevertToParent, CurrentTime);
          }
        } else if (down && (b == 4 || b == 5)) {
          io.AddMouseWheelEvent(0, b == 4 ? 1.f : -1.f);
        } else if (down && (b == 6 || b == 7)) {
          io.AddMouseWheelEvent(b == 6 ? 1.f : -1.f, 0);
        }
        any = true;
        break;
      }
      case KeyPress:
      case KeyRelease: {
        bool down = ev.type == KeyPress;
        unsigned st = ev.xkey.state;
        io.AddKeyEvent(ImGuiMod_Ctrl, (st & ControlMask) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (st & ShiftMask) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (st & Mod1Mask) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (st & Mod4Mask) != 0);
        KeySym ks = XLookupKeysym(&ev.xkey, 0);
        ImGuiKey key = mapKey(ks);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
        if (down && !(st & ControlMask)) {
          char buf[64] = {};
          int n = 0;
          KeySym sym = 0;
          if (xic_) {
            Status status = 0;
            n = Xutf8LookupString(xic_, &ev.xkey, buf, sizeof buf - 1, &sym, &status);
            if (status != XLookupChars && status != XLookupBoth) n = 0;
          } else {
            n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &sym, nullptr);
          }
          if (debug) fprintf(stderr, "  key: sym 0x%lx -> %d bytes (ic %p)\n", (unsigned long)sym, n, (void*)xic_);
          if (n > 0) {
            buf[n] = 0;
            if ((unsigned char)buf[0] >= 0x20 && buf[0] != 0x7F) io.AddInputCharactersUTF8(buf);
          }
        }
        any = true;
        break;
      }
      case FocusIn:
      case FocusOut: {
        // Pointer / inferior notifications are not real focus changes: taking the focus on a
        // click produces a FocusOut(NotifyPointer) that must not deactivate the text field the
        // click just activated.
        int detail = ev.xfocus.detail;
        if (detail == NotifyPointer || detail == NotifyInferior || detail == NotifyPointerRoot) break;
        bool in = ev.type == FocusIn;
        io.AddFocusEvent(in);
        if (xic_) {
          if (in) XSetICFocus(xic_);
          else XUnsetICFocus(xic_);
        }
        any = true;
        break;
      }
      case SelectionRequest: serveSelection(ev.xselectionrequest); break;
      case SelectionClear:
        if (ev.xselectionclear.selection == atomClipboard_) clipOwned_.clear();
        break;
      default: break;
    }
  }
  setCursor(ImGui::GetMouseCursor());
  return any;
}

void X11GuiWindow::setCursor(ImGuiMouseCursor c) {
  if (c == lastCursor_ || !win_ || gone_) return;
  lastCursor_ = c;
  if (c < 0 || c >= ImGuiMouseCursor_COUNT) c = ImGuiMouseCursor_Arrow;
  XDefineCursor(dpy_, win_, cursors_[c]);
}

bool X11GuiWindow::beginGl(int width, int height, uint32_t clearRgb) {
  if (!ctx_ || gone_) return false;
  prevDpy_ = glXGetCurrentDisplay();
  prevCtx_ = glXGetCurrentContext();
  prevDraw_ = glXGetCurrentDrawable();
  prevRead_ = glXGetCurrentReadDrawable();
  if (!glXMakeCurrent(dpy_, win_, ctx_)) return false;
  if (!swapIntervalSet_) {
    swapIntervalSet_ = true;
    // Never wait for vblank on the host's main thread.
    using SwapExt = void (*)(Display*, GLXDrawable, int);
    using SwapMesa = int (*)(unsigned);
    if (auto ext = (SwapExt)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT")) ext(dpy_, win_, 0);
    else if (auto mesa = (SwapMesa)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA")) mesa(0);
  }
  glViewport(0, 0, width, height);
  glClearColor(((clearRgb >> 16) & 0xFF) / 255.f, ((clearRgb >> 8) & 0xFF) / 255.f, (clearRgb & 0xFF) / 255.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  return true;
}

void X11GuiWindow::endGl(bool swap) {
  if (swap) glXSwapBuffers(dpy_, win_);
  if (prevCtx_ && prevDpy_) glXMakeContextCurrent(prevDpy_, prevDraw_, prevRead_, prevCtx_);
  else glXMakeCurrent(dpy_, None, nullptr);
}

void X11GuiWindow::placeEditor(int x, int y, int w, int h, bool visible) {
  if (!editor_ || gone_) return;
  if (visible && w > 0 && h > 0) {
    if (x != editorX_ || y != editorY_ || w != editorW_ || h != editorH_) {
      XMoveResizeWindow(dpy_, editor_, x, y, (unsigned)w, (unsigned)h);
      editorX_ = x;
      editorY_ = y;
      editorW_ = w;
      editorH_ = h;
    }
    if (!editorMapped_) XMapWindow(dpy_, editor_);
    editorMapped_ = true;
  } else if (editorMapped_) {
    XUnmapWindow(dpy_, editor_);
    editorMapped_ = false;
  }
  XFlush(dpy_);
}

bool X11GuiWindow::editorChildSize(int& w, int& h) {
  Window root, parent, *kids = nullptr;
  unsigned n = 0;
  bool ok = false;
  if (XQueryTree(dpy_, editor_, &root, &parent, &kids, &n) && n > 0) {
    XWindowAttributes a;
    if (XGetWindowAttributes(dpy_, kids[0], &a)) {
      w = a.width;
      h = a.height;
      ok = true;
    }
  }
  if (kids) XFree(kids);
  return ok;
}

void X11GuiWindow::revealInFileManager(const std::string& path) {
  // xdg-open shows a folder; it cannot select the file in it.
  std::string dir = dirExists(path) ? path : dirOf(path);
  const char* exe = nullptr;
  for (const char* p : {"/usr/bin/xdg-open", "/usr/local/bin/xdg-open", "/bin/xdg-open"})
    if (access(p, X_OK) == 0) exe = p;
  if (!exe) return;
  // Started like the helpers (nothing of the DAW's inherited), never waited for here; finished
  // ones are reaped on the next call.
  static std::vector<std::unique_ptr<ChildProcess>> launched;
  launched.erase(std::remove_if(launched.begin(), launched.end(), [](auto& c) { return c->exited(); }),
                 launched.end());
  auto proc = std::make_unique<ChildProcess>();
  std::string err;
  if (proc->start(exe, {dir}, SpawnOptions{}, err)) launched.push_back(std::move(proc));
}

// ---- clipboard ----

void X11GuiWindow::serveSelection(const XSelectionRequestEvent& req) {
  XSelectionEvent ev{};
  ev.type = SelectionNotify;
  ev.display = req.display;
  ev.requestor = req.requestor;
  ev.selection = req.selection;
  ev.target = req.target;
  ev.time = req.time;
  ev.property = None;
  if (req.selection == atomClipboard_ && !clipOwned_.empty()) {
    Atom prop = req.property ? req.property : req.target;
    if (req.target == atomTargets_) {
      Atom targets[] = {atomTargets_, atomUtf8_, XA_STRING};
      XChangeProperty(dpy_, req.requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char*)targets, 3);
      ev.property = prop;
    } else if (req.target == atomUtf8_ || req.target == XA_STRING) {
      XChangeProperty(dpy_, req.requestor, prop, req.target, 8, PropModeReplace,
                      (const unsigned char*)clipOwned_.data(), (int)clipOwned_.size());
      ev.property = prop;
    }
  }
  XSendEvent(dpy_, req.requestor, False, 0, (XEvent*)&ev);
  XFlush(dpy_);
}

void X11GuiWindow::setClipboard(ImGuiContext*, const char* text) {
  auto* self = static_cast<X11GuiWindow*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
  if (!self || !self->dpy_) return;
  self->clipOwned_ = text ? text : "";
  XSetSelectionOwner(self->dpy_, self->atomClipboard_, self->win_, CurrentTime);
  XFlush(self->dpy_);
}

const char* X11GuiWindow::getClipboard(ImGuiContext*) {
  auto* self = static_cast<X11GuiWindow*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
  if (!self || !self->dpy_) return "";
  if (XGetSelectionOwner(self->dpy_, self->atomClipboard_) == self->win_) return self->clipOwned_.c_str();
  self->clipGot_.clear();
  XConvertSelection(self->dpy_, self->atomClipboard_, self->atomUtf8_, self->atomProp_, self->win_, CurrentTime);
  XFlush(self->dpy_);
  uint64_t deadline = nowMs() + 300;
  while (nowMs() < deadline) {
    XEvent ev;
    if (XCheckTypedWindowEvent(self->dpy_, self->win_, SelectionNotify, &ev)) {
      if (ev.xselection.property != None) {
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(self->dpy_, self->win_, self->atomProp_, 0, 1 << 20, True, AnyPropertyType, &type,
                               &format, &n, &after, &data) == Success &&
            data) {
          self->clipGot_.assign((const char*)data, n);
          XFree(data);
        }
      }
      break;
    }
    usleep(5000);
  }
  return self->clipGot_.c_str();
}

}  // namespace

std::unique_ptr<GuiWindow> GuiWindow::create(Plugin& plugin) { return std::make_unique<X11GuiWindow>(plugin); }

void guiWindowShutdown() {
  std::lock_guard<std::mutex> lk(gErrM);
  if (!gHandlerInstalled) return;
  gHandlerInstalled = false;
  // Put the previous handler back, unless someone replaced ours since: then leave theirs.
  XErrorHandler cur = XSetErrorHandler(gPrevHandler);
  if (cur != onXError) XSetErrorHandler(cur);
}

}  // namespace yov
