// GuiWindow on Windows: a Win32 child window of the host's window with a WGL context, and a
// child window for the plugin's editor. Used by the Dear ImGui GUI when YEOLDEVST_GUI=imgui
// (the native Win32 GUI is the default on Windows).
#include "plugin/imgui/GuiWindow.h"

#include "common/Util.h"
#include "plugin/BrowserModel.h"
#include "plugin/Plugin.h"

#include <imgui.h>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <GL/gl.h>

#include <algorithm>
#include <cfloat>
#include <vector>

namespace yov {

namespace {

std::wstring classSuffix() {
  // Unique per module: the CLAP and VST3 builds may both be loaded in one process.
  wchar_t b[32];
  swprintf(b, 32, L"_%p", (void*)&classSuffix);
  return b;
}
const std::wstring kGlClass = L"YeOldeVST_ImGui" + classSuffix();
const std::wstring kEditorClass = L"YeOldeVST_ImGuiEditor" + classSuffix();

HINSTANCE moduleInstance() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&moduleInstance), &mod);
  return mod;
}

ImGuiKey mapKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
  if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
  switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_MENU: return ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
  }
}

LRESULT CALLBACK editorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_ERASEBKGND) {
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH b = CreateSolidBrush(RGB((palette::kBg >> 16) & 0xFF, (palette::kBg >> 8) & 0xFF, palette::kBg & 0xFF));
    FillRect((HDC)w, &rc, b);
    DeleteObject(b);
    return 1;
  }
  return DefWindowProcW(h, m, w, l);
}

class WglGuiWindow final : public GuiWindow {
 public:
  explicit WglGuiWindow(Plugin& p) : plugin_(p) {}
  ~WglGuiWindow() override;

  bool attach(const clap_window_t* parent, int w, int h) override;
  void resize(int w, int h) override;
  void setVisible(bool visible) override;
  float scale() const override { return scale_; }
  void startTicking(std::function<void()> tick) override;
  bool pollEvents() override {
    bool r = input_;
    input_ = false;
    return r;
  }
  bool beginGl(int width, int height, uint32_t clearRgb) override;
  void endGl(bool swap) override;
  void* editorParent() override { return editor_; }
  void placeEditor(int x, int y, int w, int h, bool visible) override;
  bool editorChildSize(int& w, int& h) override;
  void destroyEditorLeftovers() override;
  bool pickFolder(const char* title, std::string& path) override;
  void revealInFileManager(const std::string& path) override;
  int messageBox(const std::string& text, int kind, int buttons) override;

 private:
  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT onMessage(HWND, UINT, WPARAM, LPARAM);
  static void registerClasses();

  Plugin& plugin_;
  HWND win_ = nullptr, editor_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC ctx_ = nullptr;
  ImGuiContext* imgui_ = nullptr;  // the context input goes to
  float scale_ = 1.f;
  bool input_ = true;
  bool tracking_ = false;
  bool swapIntervalSet_ = false;
  std::function<void()> tick_;
  HDC prevDc_ = nullptr;
  HGLRC prevCtx_ = nullptr;
};

void WglGuiWindow::registerClasses() {
  static bool done = false;
  if (done) return;
  done = true;
  WNDCLASSEXW wc{sizeof wc};
  wc.hInstance = moduleInstance();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.style = CS_OWNDC | CS_DBLCLKS;
  wc.lpfnWndProc = wndProc;
  wc.lpszClassName = kGlClass.c_str();
  RegisterClassExW(&wc);
  wc.style = 0;
  wc.lpfnWndProc = editorProc;
  wc.lpszClassName = kEditorClass.c_str();
  RegisterClassExW(&wc);
}

WglGuiWindow::~WglGuiWindow() {
  if (win_) KillTimer(win_, 1);
  if (ctx_) {
    if (wglGetCurrentContext() == ctx_) wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(ctx_);
  }
  if (win_) {
    SetWindowLongPtrW(win_, GWLP_USERDATA, 0);
    DestroyWindow(win_);
  }
}

bool WglGuiWindow::attach(const clap_window_t* parent, int w, int h) {
  HWND p = (HWND)parent->win32;
  if (win_) {
    SetParent(win_, p);
    return true;
  }
  registerClasses();
  imgui_ = ImGui::GetCurrentContext();
  int dpi = p ? (int)GetDpiForWindow(p) : (int)GetDpiForSystem();
  scale_ = dpi > 0 ? dpi / 96.f : 1.f;
  win_ = CreateWindowExW(WS_EX_CONTROLPARENT, kGlClass.c_str(), L"YeOldeVST",
                         WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, std::max(w, 1),
                         std::max(h, 1), p, nullptr, moduleInstance(), this);
  if (!win_) return false;
  editor_ = CreateWindowExW(0, kEditorClass.c_str(), L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1, 1, win_, nullptr,
                            moduleInstance(), nullptr);
  dc_ = GetDC(win_);
  PIXELFORMATDESCRIPTOR pfd{sizeof pfd, 1};
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.iLayerType = PFD_MAIN_PLANE;
  int pf = ChoosePixelFormat(dc_, &pfd);
  if (!pf || !SetPixelFormat(dc_, pf, &pfd)) {
    logLine("gui: no OpenGL pixel format");
    return false;
  }
  ctx_ = wglCreateContext(dc_);
  if (!ctx_) {
    logLine("gui: cannot create an OpenGL context");
    return false;
  }
  return true;
}

void WglGuiWindow::resize(int w, int h) {
  if (win_) SetWindowPos(win_, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void WglGuiWindow::setVisible(bool visible) {
  if (win_) ShowWindow(win_, visible ? SW_SHOW : SW_HIDE);
}

void WglGuiWindow::startTicking(std::function<void()> tick) {
  tick_ = std::move(tick);
  SetTimer(win_, 1, 15, nullptr);
}

bool WglGuiWindow::beginGl(int width, int height, uint32_t clearRgb) {
  if (!ctx_) return false;
  prevDc_ = wglGetCurrentDC();
  prevCtx_ = wglGetCurrentContext();
  if (!wglMakeCurrent(dc_, ctx_)) return false;
  if (!swapIntervalSet_) {
    swapIntervalSet_ = true;
    // Never wait for vblank on the host's GUI thread.
    using SwapInterval = BOOL(WINAPI*)(int);
    if (auto f = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT")) f(0);
  }
  glViewport(0, 0, width, height);
  glClearColor(((clearRgb >> 16) & 0xFF) / 255.f, ((clearRgb >> 8) & 0xFF) / 255.f, (clearRgb & 0xFF) / 255.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  return true;
}

void WglGuiWindow::endGl(bool swap) {
  if (swap) SwapBuffers(dc_);
  if (prevCtx_) wglMakeCurrent(prevDc_, prevCtx_);
  else wglMakeCurrent(nullptr, nullptr);
}

void WglGuiWindow::placeEditor(int x, int y, int w, int h, bool visible) {
  if (!editor_) return;
  if (visible && w > 0 && h > 0) {
    SetWindowPos(editor_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  } else {
    ShowWindow(editor_, SW_HIDE);
  }
}

bool WglGuiWindow::editorChildSize(int& w, int& h) {
  HWND child = GetWindow(editor_, GW_CHILD);
  RECT r{};
  if (!child || !GetWindowRect(child, &r)) return false;
  w = r.right - r.left;
  h = r.bottom - r.top;
  return true;
}

void WglGuiWindow::destroyEditorLeftovers() {
  // Only our own thread's windows can be destroyed here; a bridge process cleans up its own.
  std::vector<HWND> kids;
  for (HWND c = GetWindow(editor_, GW_CHILD); c && kids.size() < 64; c = GetWindow(c, GW_HWNDNEXT)) kids.push_back(c);
  for (HWND c : kids)
    if (GetWindowThreadProcessId(c, nullptr) == GetCurrentThreadId()) DestroyWindow(c);
}

bool WglGuiWindow::pickFolder(const char* title, std::string& out) {
  bool got = false;
  HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(widen(title).c_str());
    if (SUCCEEDED(dlg->Show(GetAncestor(win_, GA_ROOT)))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          out = narrow(path);
          got = true;
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(hrInit)) CoUninitialize();
  if (!got) out.clear();
  return true;  // a cancelled dialog is handled too: no fallback picker on Windows
}

void WglGuiWindow::revealInFileManager(const std::string& path) {
  std::wstring args = L"/select,\"" + widen(path) + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

int WglGuiWindow::messageBox(const std::string& text, int kind, int buttons) {
  UINT type = (buttons == 2 ? MB_YESNOCANCEL : buttons == 1 ? MB_OKCANCEL : MB_OK) |
              (kind == 2 ? MB_ICONERROR : kind == 1 ? MB_ICONWARNING : MB_ICONINFORMATION);
  int r = MessageBoxW(GetAncestor(win_, GA_ROOT), widen(text).c_str(), L"YeOldeVST", type);
  return r == IDYES || r == IDOK ? 0 : r == IDNO ? 1 : 2;
}

LRESULT CALLBACK WglGuiWindow::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
  }
  auto self = reinterpret_cast<WglGuiWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->onMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT WglGuiWindow::onMessage(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_TIMER) {
    if (w == 1 && tick_) tick_();
    return 0;
  }
  if (!imgui_) return DefWindowProcW(h, m, w, l);
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  LRESULT result = 0;
  bool handled = true;
  auto modifiers = [&] {
    io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
  };
  switch (m) {
    case WM_ERASEBKGND: result = 1; break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      EndPaint(h, &ps);
      input_ = true;
      break;
    }
    case WM_MOUSEMOVE:
      if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
      }
      io.AddMousePosEvent((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
      input_ = true;
      break;
    case WM_MOUSELEAVE:
      tracking_ = false;
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
      input_ = true;
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK: {
      int b = (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) ? 0 : (m == WM_RBUTTONDOWN || m == WM_RBUTTONDBLCLK) ? 1 : 2;
      if (GetFocus() != h) SetFocus(h);  // keys go to the focused window
      if (!GetCapture()) SetCapture(h);
      io.AddMousePosEvent((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
      io.AddMouseButtonEvent(b, true);
      input_ = true;
      break;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
      int b = m == WM_LBUTTONUP ? 0 : m == WM_RBUTTONUP ? 1 : 2;
      io.AddMouseButtonEvent(b, false);
      if (GetCapture() == h && !(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
      input_ = true;
      break;
    }
    case WM_MOUSEWHEEL:
      io.AddMouseWheelEvent(0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
      input_ = true;
      break;
    case WM_MOUSEHWHEEL:
      io.AddMouseWheelEvent(-(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA, 0);
      input_ = true;
      break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
      modifiers();
      ImGuiKey k = mapKey(w);
      if (k != ImGuiKey_None) io.AddKeyEvent(k, m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
      input_ = true;
      handled = m == WM_KEYDOWN || m == WM_KEYUP;
      break;
    }
    case WM_CHAR:
      if (w >= 0x20 && w != 0x7F) io.AddInputCharacterUTF16((unsigned short)w);
      input_ = true;
      break;
    case WM_GETDLGCODE: result = DLGC_WANTALLKEYS | DLGC_WANTCHARS; break;
    case WM_SETFOCUS:
      io.AddFocusEvent(true);
      input_ = true;
      break;
    case WM_KILLFOCUS:
      io.AddFocusEvent(false);
      input_ = true;
      break;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        LPCWSTR c = IDC_ARROW;
        switch (ImGui::GetMouseCursor()) {
          case ImGuiMouseCursor_TextInput: c = IDC_IBEAM; break;
          case ImGuiMouseCursor_ResizeAll: c = IDC_SIZEALL; break;
          case ImGuiMouseCursor_ResizeEW: c = IDC_SIZEWE; break;
          case ImGuiMouseCursor_ResizeNS: c = IDC_SIZENS; break;
          case ImGuiMouseCursor_ResizeNESW: c = IDC_SIZENESW; break;
          case ImGuiMouseCursor_ResizeNWSE: c = IDC_SIZENWSE; break;
          case ImGuiMouseCursor_Hand: c = IDC_HAND; break;
          case ImGuiMouseCursor_NotAllowed: c = IDC_NO; break;
          default: break;
        }
        SetCursor(LoadCursorW(nullptr, c));
        result = TRUE;
      } else {
        handled = false;
      }
      break;
    case WM_DPICHANGED_AFTERPARENT: {
      int d = (int)GetDpiForWindow(h);
      if (d > 0) scale_ = d / 96.f;
      input_ = true;
      break;
    }
    default: handled = false; break;
  }
  ImGui::SetCurrentContext(prev);
  return handled ? result : DefWindowProcW(h, m, w, l);
}

}  // namespace

std::unique_ptr<GuiWindow> GuiWindow::create(Plugin& plugin) { return std::make_unique<WglGuiWindow>(plugin); }

void guiWindowShutdown() {}

}  // namespace yov
