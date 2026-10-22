#include "platform/Helper.h"

#include "common/Util.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <vector>

namespace yov::helper {

namespace {

LRESULT CALLBACK containerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_ERASEBKGND) return 1;
  return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK probeWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

const wchar_t* containerClass() {
  static bool registered = [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = containerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"YeOldeVSTBridgeEditor";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    return true;
  }();
  (void)registered;
  return L"YeOldeVSTBridgeEditor";
}

// ---- popup eater ----
// During a scan nobody is there to click a plugin's nag screen, licence prompt or error box. A
// watchdog thread finds top-level windows this process opens (other than our editor test
// window), reports their text, and dismisses them so the probe can carry on instead of hanging.
// Only the scanner does this; a plugin loaded for real shows its dialogs as usual.

HWND gProbeWindow = nullptr;
void (*gReport)(const std::string&) = nullptr;

struct PopupScan {
  DWORD pid;
  std::vector<HWND> found;
};

BOOL CALLBACK collectPopups(HWND h, LPARAM lp) {
  auto* ps = reinterpret_cast<PopupScan*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != ps->pid || h == gProbeWindow || !IsWindowVisible(h)) return TRUE;
  if (GetWindow(h, GW_OWNER) == gProbeWindow && gProbeWindow) {
    // Owned by the editor test window: could be part of the editor (tool palette); still a popup
    // if it is a dialog.
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"#32770") != 0) return TRUE;
  }
  ps->found.push_back(h);
  return TRUE;
}

BOOL CALLBACK collectText(HWND h, LPARAM lp) {
  auto* out = reinterpret_cast<std::wstring*>(lp);
  wchar_t cls[32] = {};
  GetClassNameW(h, cls, 32);
  if (_wcsicmp(cls, L"Static") == 0 || _wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit20W") == 0) {
    wchar_t t[512] = {};
    GetWindowTextW(h, t, 512);
    if (t[0]) {
      if (!out->empty()) *out += L" ";
      *out += t;
    }
  }
  return out->size() < 400;
}

void popupEater() {
  std::map<HWND, int> attempts;
  for (;;) {
    Sleep(250);
    PopupScan ps{GetCurrentProcessId(), {}};
    EnumWindows(collectPopups, (LPARAM)&ps);
    for (HWND h : ps.found) {
      int& n = attempts[h];
      if (n == 0) {
        wchar_t title[256] = {};
        GetWindowTextW(h, title, 256);
        std::wstring text;
        EnumChildWindows(h, collectText, (LPARAM)&text);
        std::string desc = narrow(title);
        if (!text.empty()) desc += (desc.empty() ? "" : ": ") + narrow(text);
        if (desc.empty()) desc = "(untitled window)";
        if (desc.size() > 300) desc = desc.substr(0, 300) + "...";
        if (gReport) gReport(desc);
      }
      // Escalate: Cancel, then No / OK, then close.
      wchar_t cls[64] = {};
      GetClassNameW(h, cls, 64);
      bool dialog = wcscmp(cls, L"#32770") == 0;
      if (dialog && n == 0) PostMessageW(h, WM_COMMAND, IDCANCEL, 0);
      else if (dialog && n == 1) PostMessageW(h, WM_COMMAND, IDNO, 0);
      else if (dialog && n == 2) PostMessageW(h, WM_COMMAND, IDOK, 0);
      else PostMessageW(h, WM_CLOSE, 0, 0);
      ++n;
    }
  }
}

}  // namespace

void init(bool probe) {
  SetErrorMode(probe ? SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
                     : SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
}

void writeStdout(const std::string& s) {
  static HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD w;
  WriteFile(out, s.data(), (DWORD)s.size(), &w, nullptr);
  FlushFileBuffers(out);
}

void exitNow(int code) {
  TerminateProcess(GetCurrentProcess(), (UINT)code);
  for (;;) Sleep(1000);
}

void boostAudioThread() {
  // Ask the scheduler for audio-thread treatment.
  if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
    using Fn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    if (auto f = (Fn)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) {
      DWORD idx = 0;
      f(L"Pro Audio", &idx);
    }
  }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

void pumpMessages(int budget) {
  MSG msg;
  while (budget-- > 0 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

bool haveDisplay() { return true; }

void* containerCreate(int64_t parent, int w, int h) {
  return CreateWindowExW(WS_EX_NOPARENTNOTIFY, containerClass(), L"",
                         WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, std::max(w, 1),
                         std::max(h, 1), (HWND)(intptr_t)parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void containerResize(void* c, int w, int h) {
  if (c) SetWindowPos((HWND)c, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool containerChildSize(void* c, int& w, int& h) {
  HWND child = GetWindow((HWND)c, GW_CHILD);
  RECT r{};
  if (!child || !GetWindowRect(child, &r)) return false;
  w = r.right - r.left;
  h = r.bottom - r.top;
  return true;
}

void containerDestroy(void* c) {
  if (c) DestroyWindow((HWND)c);
}

void* probeWindowCreate() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = probeWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"YeOldeVSTProbe";
  RegisterClassW(&wc);
  // Real hosts show the parent window; some editors fail with an invisible one. Show it far
  // off-screen, without activating it, so nothing flashes on the user's desktop.
  HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"probe", WS_POPUP, -32000, -32000,
                             1600, 1200, nullptr, nullptr, wc.hInstance, nullptr);
  gProbeWindow = wnd;
  ShowWindow(wnd, SW_SHOWNOACTIVATE);
  return wnd;
}

void probeWindowDestroy(void* w) {
  if (w) DestroyWindow((HWND)w);
}

void startPopupEater(void (*report)(const std::string&)) {
  gReport = report;
  std::thread(popupEater).detach();  // the process ends via exitNow, no join needed
}

}  // namespace yov::helper
