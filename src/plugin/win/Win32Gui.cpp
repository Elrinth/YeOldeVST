// Win32 UI for YeOldeVST. Plain Win32 + common controls, dark themed, DPI aware.
#define ISOLATION_AWARE_ENABLED 1  // use comctl32 v6 through our own manifest (we are a DLL)

#include "plugin/win/Win32Gui.h"

#include "common/Util.h"
#include "plugin/Plugin.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>

namespace yov {

namespace {

// ---- palette ----
constexpr COLORREF kBg = RGB(0x1C, 0x1D, 0x21);
constexpr COLORREF kPanel = RGB(0x25, 0x27, 0x2C);
constexpr COLORREF kCtrl = RGB(0x2F, 0x32, 0x38);
constexpr COLORREF kCtrlHot = RGB(0x3A, 0x3D, 0x45);
constexpr COLORREF kBorder = RGB(0x40, 0x43, 0x4A);
constexpr COLORREF kText = RGB(0xE3, 0xE5, 0xE8);
constexpr COLORREF kDim = RGB(0x95, 0x9A, 0xA4);
constexpr COLORREF kAccent = RGB(0xD9, 0xA4, 0x41);  // old brass
constexpr COLORREF kAccentText = RGB(0x1C, 0x1D, 0x21);
constexpr COLORREF kOk = RGB(0x5C, 0xC7, 0x6E);
constexpr COLORREF kWarn = RGB(0xE8, 0xBB, 0x3A);
constexpr COLORREF kBad = RGB(0xEB, 0x5B, 0x52);
constexpr COLORREF kGrey = RGB(0x7C, 0x81, 0x8A);
constexpr COLORREF kBlue = RGB(0x6C, 0xB4, 0xEE);
constexpr COLORREF kRowAlt = RGB(0x22, 0x24, 0x29);
constexpr COLORREF kSelect = RGB(0x4A, 0x3E, 0x24);

enum : int {
  ID_BROWSE = 100,
  ID_PROGRAM,
  ID_VIEW,
  ID_UNLOAD,
  ID_RELOAD,
  ID_FOLDERS,
  ID_ADD_FOLDER,
  ID_REMOVE_FOLDER,
  ID_SCAN,
  ID_RESCAN,
  ID_RETRY,
  ID_STOP,
  ID_TGL_EDITORS,
  ID_TGL_HIDDEN,
  ID_SEARCH,
  ID_FILTER,
  ID_LIST,
  ID_LOAD,
  ID_LOAD_SANDBOXED,
  ID_CTX_LOAD = 300,
  ID_CTX_LOAD_SANDBOXED,
  ID_CTX_RESCAN,
  ID_CTX_EXPLORE,
  ID_CTX_COPY,
  ID_CTX_TOGGLE,
};

constexpr UINT_PTR kTimerId = 1;
constexpr int kToolbarH = 40;
constexpr int kGenericRowH = 26;

enum Filter { F_ALL, F_WORKING, F_PROBLEMS, F_INSTRUMENTS, F_EFFECTS, F_32BIT, F_OTHER, F_COUNT };
const wchar_t* kFilterNames[F_COUNT] = {L"All plugins",      L"Working",           L"Problems",
                                        L"Instruments",      L"Effects",           L"32-bit only",
                                        L"Not VST2 / other"};

const wchar_t* kColumns[] = {L"Name", L"Vendor", L"Type", L"Bits", L"Status", L"Editor", L"Details", L"File"};
const int kColWidths[] = {230, 130, 90, 52, 76, 74, 280, 360};

std::wstring classSuffix() {
  // Unique per module: the CLAP and VST3 builds may both be loaded in one process.
  wchar_t b[32];
  swprintf(b, 32, L"_%p", (void*)&classSuffix);
  return b;
}
const std::wstring kRootClass = L"YeOldeVST_Root" + classSuffix();
const std::wstring kPageClass = L"YeOldeVST_Page" + classSuffix();
const std::wstring kGenericClass = L"YeOldeVST_Generic" + classSuffix();
const std::wstring kEditorClass = L"YeOldeVST_Editor" + classSuffix();
bool gClassesRegistered = false;

HINSTANCE moduleInstance() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&moduleInstance), &mod);
  return mod;
}

LRESULT CALLBACK editorHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_ERASEBKGND) {
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH b = CreateSolidBrush(kBg);
    FillRect((HDC)w, &rc, b);
    DeleteObject(b);
    return 1;
  }
  return DefWindowProcW(h, m, w, l);
}

void registerClasses() {
  if (gClassesRegistered) return;
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  HINSTANCE hi = moduleInstance();
  WNDCLASSEXW wc{sizeof wc};
  wc.hInstance = hi;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = nullptr;
  auto reg = [&](const std::wstring& name, WNDPROC proc) {
    wc.lpszClassName = name.c_str();
    wc.lpfnWndProc = proc;
    RegisterClassExW(&wc);
  };
  reg(kRootClass, nullptr);  // patched below
  gClassesRegistered = true;
}

std::wstring W(const std::string& s) { return widen(s); }

const wchar_t* statusLabel(ScanStatus s) {
  switch (s) {
    case ScanStatus::Ok: return L"OK";
    case ScanStatus::Warning: return L"Warning";
    case ScanStatus::Crashed: return L"Crashed";
    case ScanStatus::Timeout: return L"Hangs";
    case ScanStatus::LoadFailed: return L"Won't load";
    case ScanStatus::NotPlugin: return L"Not VST2";
    case ScanStatus::Unsupported: return L"Unsupported";
    case ScanStatus::NoHelper: return L"Needs helper";
    case ScanStatus::Shell: return L"Shell";
    default: return L"Not scanned";
  }
}

COLORREF statusColor(ScanStatus s) {
  switch (s) {
    case ScanStatus::Ok: return kOk;
    case ScanStatus::Warning: return kWarn;
    case ScanStatus::Crashed:
    case ScanStatus::Timeout:
    case ScanStatus::LoadFailed: return kBad;
    case ScanStatus::NoHelper: return kBlue;
    default: return kGrey;
  }
}

void fillRect(HDC dc, const RECT& r, COLORREF c) {
  SetDCBrushColor(dc, c);
  FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void roundRect(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius) {
  SetDCBrushColor(dc, fill);
  SetDCPenColor(dc, border);
  HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
  HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
  RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  SelectObject(dc, ob);
  SelectObject(dc, op);
}

void drawText(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt) {
  HGDIOBJ of = SelectObject(dc, f);
  SetTextColor(dc, c);
  SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
  SelectObject(dc, of);
}

// Suggest the usual VST2 folders that exist on this machine.
std::vector<std::string> defaultVstFolders() {
  std::vector<std::string> out;
  auto add = [&](const std::string& p) {
    if (p.empty() || !dirExists(p)) return;
    for (auto& o : out)
      if (foldCase(o) == foldCase(p)) return;
    out.push_back(p);
  };
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
      HKEY k;
      if (RegOpenKeyExW(root, L"SOFTWARE\\VST", 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024];
        DWORD sz = sizeof buf, type = 0;
        if (RegQueryValueExW(k, L"VSTPluginsPath", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
          buf[std::min<DWORD>(sz / 2, 1023)] = 0;
          add(trim(narrow(buf)));
        }
        RegCloseKey(k);
      }
    }
  }
  for (const char* p : {"C:\\Program Files\\VstPlugins", "C:\\Program Files\\Steinberg\\VstPlugins",
                        "C:\\Program Files\\Common Files\\VST2", "C:\\Program Files\\Common Files\\Steinberg\\VST2",
                        "C:\\Program Files (x86)\\VstPlugins", "C:\\Program Files (x86)\\Steinberg\\VstPlugins",
                        "C:\\Program Files (x86)\\Common Files\\VST2"})
    add(p);
  return out;
}

// Removes windows a plugin left in the editor host after effEditClose. Only our own thread's
// windows can be destroyed here; a bridge process cleans up its own.
void destroyLeftovers(HWND host) {
  std::vector<HWND> kids;
  for (HWND c = GetWindow(host, GW_CHILD); c && kids.size() < 64; c = GetWindow(c, GW_HWNDNEXT)) kids.push_back(c);
  for (HWND c : kids)
    if (GetWindowThreadProcessId(c, nullptr) == GetCurrentThreadId()) DestroyWindow(c);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void Win32Gui::unregisterClasses() {
  if (!gClassesRegistered) return;
  HINSTANCE hi = moduleInstance();
  UnregisterClassW(kRootClass.c_str(), hi);
  UnregisterClassW(kPageClass.c_str(), hi);
  UnregisterClassW(kGenericClass.c_str(), hi);
  UnregisterClassW(kEditorClass.c_str(), hi);
  gClassesRegistered = false;
}

Win32Gui::Win32Gui(Plugin& plugin) : plugin_(plugin) {
  if (!gClassesRegistered) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HINSTANCE hi = moduleInstance();
    WNDCLASSEXW wc{sizeof wc};
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = kRootClass.c_str();
    wc.lpfnWndProc = rootProc;
    RegisterClassExW(&wc);
    wc.lpszClassName = kPageClass.c_str();
    wc.lpfnWndProc = pageProc;
    RegisterClassExW(&wc);
    wc.lpszClassName = kGenericClass.c_str();
    wc.lpfnWndProc = genericProc;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);
    wc.lpszClassName = kEditorClass.c_str();
    wc.lpfnWndProc = editorHostProc;
    wc.style = 0;
    RegisterClassExW(&wc);
    gClassesRegistered = true;
  }
  dpi_ = (int)GetDpiForSystem();
  browserForced_ = plugin_.hosted() == nullptr;
  preferredSize(width_, height_);
}

Win32Gui::~Win32Gui() {
  closeEditor();
  if (root_) {
    KillTimer(root_, kTimerId);
    DestroyWindow(root_);
  }
  for (HFONT f : {font_, fontBold_, fontTitle_, fontSmall_})
    if (f) DeleteObject(f);
  for (HBRUSH b : {brBg_, brPanel_, brCtrl_})
    if (b) DeleteObject(b);
}

void Win32Gui::createFonts() {
  for (HFONT f : {font_, fontBold_, fontTitle_, fontSmall_})
    if (f) DeleteObject(f);
  auto mk = [&](int pt, int weight) {
    return CreateFontW(-MulDiv(pt, dpi_, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  };
  font_ = mk(9, FW_NORMAL);
  fontBold_ = mk(9, FW_SEMIBOLD);
  fontTitle_ = mk(11, FW_SEMIBOLD);
  fontSmall_ = mk(8, FW_NORMAL);
  if (!brBg_) {
    brBg_ = CreateSolidBrush(kBg);
    brPanel_ = CreateSolidBrush(kPanel);
    brCtrl_ = CreateSolidBrush(kCtrl);
  }
}

bool Win32Gui::setParent(const clap_window_t* window) {
  HWND parent = (HWND)window->win32;
  parent_ = parent;
  if (root_) {
    SetParent(root_, parent);
    return true;
  }
  dpi_ = parent ? (int)GetDpiForWindow(parent) : (int)GetDpiForSystem();
  if (dpi_ <= 0) dpi_ = 96;
  createFonts();
  preferredSize(width_, height_);
  root_ = CreateWindowExW(WS_EX_CONTROLPARENT, kRootClass.c_str(), L"YeOldeVST",
                          WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, (int)width_, (int)height_,
                          parent, nullptr, moduleInstance(), this);
  if (!root_) return false;
  createControls();
  refreshFolders();
  refreshList();
  refreshPrograms();
  applyPage();
  SetTimer(root_, kTimerId, 30, nullptr);
  return true;
}

void Win32Gui::getSize(uint32_t& w, uint32_t& h) const {
  w = width_;
  h = height_;
}

void Win32Gui::adjustSize(uint32_t& w, uint32_t& h) const {
  if (page_now_ == Page::Editor && editorW_ > 0) {
    // The VST editor has a fixed size.
    uint32_t pw, ph;
    preferredSize(pw, ph);
    w = pw;
    h = ph;
    return;
  }
  w = std::max<uint32_t>(w, (uint32_t)S(560));
  h = std::max<uint32_t>(h, (uint32_t)S(360));
}

bool Win32Gui::setSize(uint32_t w, uint32_t h) {
  width_ = w;
  height_ = h;
  if (root_) SetWindowPos(root_, nullptr, 0, 0, (int)w, (int)h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
  layout();
  return true;
}

void Win32Gui::show() {
  if (root_) ShowWindow(root_, SW_SHOW);
}

void Win32Gui::hide() {
  if (root_) ShowWindow(root_, SW_HIDE);
}

void Win32Gui::preferredSize(uint32_t& w, uint32_t& h) const {
  Page p = currentPage();
  int minToolbarW = S(560);
  switch (p) {
    case Page::Editor:
      if (editorW_ > 0 && editorH_ > 0) {
        w = (uint32_t)std::max(editorW_, minToolbarW);
        h = (uint32_t)(editorH_ + S(kToolbarH));
        return;
      }
      w = (uint32_t)S(640);
      h = (uint32_t)S(480);
      return;
    case Page::Generic:
      w = (uint32_t)S(640);
      h = (uint32_t)S(560);
      return;
    case Page::Message:
      w = (uint32_t)S(640);
      h = (uint32_t)S(300);
      return;
    default:
      w = (uint32_t)S(960);
      h = (uint32_t)S(640);
      return;
  }
}

void Win32Gui::requestSize() {
  uint32_t w, h;
  preferredSize(w, h);
  if (w == width_ && h == height_) return;
  // Ask the host; most hosts resize the window and call setSize(). Lay out at the new size
  // either way, so the content is right even if the host does not follow.
  plugin_.requestResize(w, h);
  setSize(w, h);
}

// ---------------------------------------------------------------------------------------------
// Controls

void Win32Gui::createControls() {
  HINSTANCE hi = moduleInstance();
  auto button = [&](HWND parent, const wchar_t* text, int id) {
    HWND b = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)font_, FALSE);
    return b;
  };
  auto combo = [&](HWND parent, int id) {
    HWND c = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                             0, 0, 10, S(300), parent, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font_, FALSE);
    SetWindowTheme(c, L"DarkMode_CFD", nullptr);
    return c;
  };

  // toolbar
  btnBrowse_ = button(root_, L"Plugins", ID_BROWSE);
  cmbProgram_ = combo(root_, ID_PROGRAM);
  btnView_ = button(root_, L"Generic UI", ID_VIEW);
  btnUnload_ = button(root_, L"Unload", ID_UNLOAD);
  btnReload_ = button(root_, L"Reload plugin", ID_RELOAD);

  // pages
  page_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClass.c_str(), L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10,
                          root_, nullptr, hi, this);
  editorHost_ = CreateWindowExW(0, kEditorClass.c_str(), L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, root_,
                                nullptr, hi, nullptr);
  generic_ = CreateWindowExW(0, kGenericClass.c_str(), L"", WS_CHILD | WS_VSCROLL, 0, 0, 10, 10, root_, nullptr, hi,
                             this);

  // browser page
  lstFolders_ = CreateWindowExW(0, WC_LISTBOXW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                0, 0, 10, 10, page_, (HMENU)(INT_PTR)ID_FOLDERS, hi, nullptr);
  SendMessageW(lstFolders_, WM_SETFONT, (WPARAM)font_, FALSE);
  SetWindowTheme(lstFolders_, L"DarkMode_Explorer", nullptr);
  btnAddFolder_ = button(page_, L"Add folder...", ID_ADD_FOLDER);
  btnRemoveFolder_ = button(page_, L"Remove", ID_REMOVE_FOLDER);
  btnScan_ = button(page_, L"Scan", ID_SCAN);
  btnRescan_ = button(page_, L"Rescan all", ID_RESCAN);
  btnRetry_ = button(page_, L"Rescan failed", ID_RETRY);
  btnStop_ = button(page_, L"Stop", ID_STOP);
  tglEditors_ = button(page_, L"Test editors", ID_TGL_EDITORS);
  tglHidden_ = button(page_, L"Hide plugin popups during scan", ID_TGL_HIDDEN);
  auto& st = Shared::get().settings;
  toggles_[tglEditors_] = st.testEditors;
  toggles_[tglHidden_] = st.hiddenDesktop;

  edtSearch_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                               page_, (HMENU)(INT_PTR)ID_SEARCH, hi, nullptr);
  SendMessageW(edtSearch_, WM_SETFONT, (WPARAM)font_, FALSE);
  SendMessageW(edtSearch_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search name, vendor, file...");
  SendMessageW(edtSearch_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
  cmbFilter_ = combo(page_, ID_FILTER);
  for (auto f : kFilterNames) SendMessageW(cmbFilter_, CB_ADDSTRING, 0, (LPARAM)f);
  SendMessageW(cmbFilter_, CB_SETCURSEL, plugin_.isInstrument() ? F_INSTRUMENTS : F_EFFECTS, 0);

  lv_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
                            LVS_SHOWSELALWAYS,
                        0, 0, 10, 10, page_, (HMENU)(INT_PTR)ID_LIST, hi, nullptr);
  SendMessageW(lv_, WM_SETFONT, (WPARAM)font_, FALSE);
  ListView_SetExtendedListViewStyle(lv_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP |
                                             LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
  SetWindowTheme(lv_, L"DarkMode_Explorer", nullptr);
  if (HWND hdr = ListView_GetHeader(lv_)) SetWindowTheme(hdr, L"DarkMode_ItemsView", nullptr);
  // The header reports its drawing to the list view, not to us: catch it there.
  SetWindowSubclass(lv_, listSubclass, 1, (DWORD_PTR)this);
  ListView_SetBkColor(lv_, kPanel);
  ListView_SetTextBkColor(lv_, kPanel);
  ListView_SetTextColor(lv_, kText);
  for (int i = 0; i < (int)std::size(kColumns); ++i) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)kColumns[i];
    c.cx = S(kColWidths[i]);
    c.iSubItem = i;
    ListView_InsertColumn(lv_, i, &c);
  }
  btnLoad_ = button(page_, L"Load", ID_LOAD);
  btnLoadSandboxed_ = button(page_, L"Load sandboxed", ID_LOAD_SANDBOXED);

  // Tooltips for the less obvious buttons.
  HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0,
                             0, root_, nullptr, hi, nullptr);
  SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, S(360));
  auto addTip = [&](HWND ctl, const wchar_t* text) {
    TOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(ctl);
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  };
  addTip(btnScan_, L"Scan new and changed plugins in the folders above. Each plugin is tested in its own "
                   L"throwaway process, so a crashing plugin cannot take your DAW down.");
  addTip(btnRescan_, L"Forget all results and test every plugin again.");
  addTip(btnRetry_, L"Test again only the plugins that crashed, hung or failed to load (including ones whose "
                    L"editor crashed). Everything else keeps its result.");
  addTip(tglEditors_, L"Also open and close each plugin's editor window during the scan. Slower, but catches "
                      L"plugins whose UI crashes. Editors that already passed are not tested again; with this "
                      L"off, the Editor column shows 'not tested' for plugins never tested.");
  addTip(tglHidden_, L"Run the scan on a hidden desktop so nag screens and licence dialogs don't pop up. "
                     L"Plugins that wait for such a dialog are reported as 'Hangs'.");
  addTip(btnLoadSandboxed_, L"Run the plugin in a separate helper process. If it crashes, only that process dies: "
                            L"your DAW keeps running and you can reload it. 32-bit plugins always run this way.");
  addTip(btnReload_, L"Start the plugin again and restore its last saved settings.");
}

void Win32Gui::layout() {
  if (!root_) return;
  RECT rc;
  GetClientRect(root_, &rc);
  int w = rc.right, h = rc.bottom;
  int tb = S(kToolbarH);
  int pad = S(8);
  int bh = S(26);
  int y = (tb - bh) / 2;

  Page p = currentPage();
  bool loaded = plugin_.hosted() != nullptr;
  // toolbar: [Plugins] title ... [program] [view] [unload]
  int x = pad;
  SetWindowPos(btnBrowse_, nullptr, x, y, S(96), bh, SWP_NOZORDER);
  x += S(96) + pad;
  int right = w - pad;
  auto placeRight = [&](HWND ctl, int cw, bool visible) {
    ShowWindow(ctl, visible ? SW_SHOW : SW_HIDE);
    if (!visible) return;
    right -= cw;
    SetWindowPos(ctl, nullptr, right, y, cw, bh, SWP_NOZORDER);
    right -= S(6);
  };
  placeRight(btnUnload_, S(70), loaded && p != Page::Browser);
  bool canToggle = loaded && plugin_.hosted()->info().hasEditor && !plugin_.hosted()->dead() && p != Page::Browser;
  placeRight(btnView_, S(92), canToggle);
  bool programs = loaded && !plugin_.hosted()->dead() && plugin_.hosted()->info().numPrograms > 1 &&
                  p != Page::Browser;
  int comboW = std::clamp(w / 4, S(120), S(220));
  ShowWindow(cmbProgram_, programs ? SW_SHOW : SW_HIDE);
  if (programs) {
    right -= comboW;
    SetWindowPos(cmbProgram_, nullptr, right, y + 1, comboW, S(400), SWP_NOZORDER);
    right -= S(6);
  }

  RECT body{0, tb, w, h};
  ShowWindow(page_, p == Page::Browser ? SW_SHOW : SW_HIDE);
  ShowWindow(generic_, p == Page::Generic ? SW_SHOW : SW_HIDE);
  ShowWindow(editorHost_, p == Page::Editor ? SW_SHOW : SW_HIDE);
  ShowWindow(btnReload_, p == Page::Message && loaded ? SW_SHOW : SW_HIDE);
  if (p == Page::Browser) {
    SetWindowPos(page_, nullptr, 0, tb, w, h - tb, SWP_NOZORDER);
    layoutBrowser(w, h - tb);
  } else if (p == Page::Generic) {
    SetWindowPos(generic_, nullptr, 0, tb, w, h - tb, SWP_NOZORDER);
    genericUpdateScroll();
  } else if (p == Page::Editor) {
    int ew = editorW_ > 0 ? editorW_ : w;
    int eh = editorH_ > 0 ? editorH_ : h - tb;
    SetWindowPos(editorHost_, nullptr, std::max(0, (w - ew) / 2), tb, ew, eh, SWP_NOZORDER);
  } else if (p == Page::Message) {
    SetWindowPos(btnReload_, nullptr, (w - S(140)) / 2, tb + (h - tb) / 2 + S(30), S(140), S(30), SWP_NOZORDER);
  }
  InvalidateRect(root_, nullptr, TRUE);
}

void Win32Gui::layoutBrowser(int w, int h) {
  int pad = S(10);
  int bh = S(26);
  int btnW = S(110);
  int y = pad;
  foldersLabelRect_ = {pad, y, w - pad, y + S(18)};
  y += S(20);
  int foldersH = S(70);
  SetWindowPos(lstFolders_, nullptr, pad, y, w - pad * 3 - btnW, foldersH, SWP_NOZORDER);
  SetWindowPos(btnAddFolder_, nullptr, w - pad - btnW, y, btnW, bh, SWP_NOZORDER);
  SetWindowPos(btnRemoveFolder_, nullptr, w - pad - btnW, y + bh + S(6), btnW, bh, SWP_NOZORDER);
  y += foldersH + S(8);

  int x = pad;
  for (HWND b : {btnScan_, btnRescan_, btnRetry_}) {
    SetWindowPos(b, nullptr, x, y, S(96), bh, SWP_NOZORDER);
    x += S(96) + S(6);
  }
  SetWindowPos(btnStop_, nullptr, x, y, S(64), bh, SWP_NOZORDER);
  x += S(64) + S(16);
  int t1 = S(116), t2 = S(230);
  if (x + t1 + t2 + pad <= w) {
    ShowWindow(tglEditors_, SW_SHOW);
    ShowWindow(tglHidden_, SW_SHOW);
    SetWindowPos(tglEditors_, nullptr, x, y, t1, bh, SWP_NOZORDER);
    SetWindowPos(tglHidden_, nullptr, x + t1 + S(4), y, t2, bh, SWP_NOZORDER);
  } else {
    ShowWindow(tglEditors_, SW_HIDE);
    ShowWindow(tglHidden_, SW_HIDE);
  }
  y += bh + S(10);

  int filterW = S(150);
  SetWindowPos(edtSearch_, nullptr, pad, y, w - pad * 3 - filterW, bh, SWP_NOZORDER);
  SetWindowPos(cmbFilter_, nullptr, w - pad - filterW, y + 1, filterW, S(300), SWP_NOZORDER);
  y += bh + S(8);

  int bottomH = S(40);
  SetWindowPos(lv_, nullptr, pad, y, w - pad * 2, std::max(S(40), h - y - bottomH), SWP_NOZORDER);
  int by = h - bottomH + (bottomH - bh) / 2;
  int lw = S(90), sw = S(130);
  SetWindowPos(btnLoad_, nullptr, w - pad - lw, by, lw, bh, SWP_NOZORDER);
  SetWindowPos(btnLoadSandboxed_, nullptr, w - pad - lw - S(6) - sw, by, sw, bh, SWP_NOZORDER);
  int statusRight = w - pad - lw - S(6) - sw - S(12);
  progressRect_ = {pad, by + bh - S(4), std::max(pad, statusRight), by + bh};
  statusRect_ = {pad, by - S(2), std::max(pad, statusRight), by + bh - S(6)};
  InvalidateRect(page_, nullptr, TRUE);
}

Win32Gui::Page Win32Gui::currentPage() const {
  auto h = plugin_.hosted();
  if (browserForced_) return Page::Browser;
  if (!h) return plugin_.hasMissingPlugin() ? Page::Message : Page::Browser;
  if (h->dead()) return Page::Message;
  if (plugin_.genericView() || !h->info().hasEditor) return Page::Generic;
  return Page::Editor;
}

void Win32Gui::applyPage() {
  Page p = currentPage();
  if (p != Page::Editor && editorOpen_) closeEditor();
  page_now_ = p;
  SetWindowTextW(btnBrowse_, p == Page::Browser && plugin_.hosted() ? L"Back" : L"Plugins");
  SetWindowTextW(btnView_, plugin_.genericView() ? L"Plugin UI" : L"Generic UI");
  if (p == Page::Editor && !editorOpen_) openEditor();
  if (p == Page::Generic) {
    genValues_.clear();
    genTexts_.clear();
    genScroll_ = 0;
  }
  layout();
  requestSize();
  if (p == Page::Browser) SetFocus(lv_);
}

// ---------------------------------------------------------------------------------------------
// Editor

void Win32Gui::openEditor() {
  auto h = plugin_.hosted();
  if (!h || editorOpen_ || !editorHost_) return;
  int w = 0, hh = 0;
  h->editorSize(w, hh);
  if (w > 0 && hh > 0) {
    editorW_ = w;
    editorH_ = hh;
    SetWindowPos(editorHost_, nullptr, 0, 0, w, hh, SWP_NOMOVE | SWP_NOZORDER);
  }
  ShowWindow(editorHost_, SW_SHOW);
  if (!h->editorOpen(editorHost_)) {
    if (h->dead()) {
      onCrash();
      return;
    }
    plugin_.setGenericView(true);
    applyPage();
    return;
  }
  editorOpen_ = true;
  // Many plugins only know their real size once open.
  if (h->editorSize(w, hh) && w > 0 && hh > 0) {
    editorW_ = w;
    editorH_ = hh;
  }
  if (editorW_ <= 0 || editorH_ <= 0) {
    // No size reported: use the size of whatever window the plugin created.
    HWND child = GetWindow(editorHost_, GW_CHILD);
    RECT r{};
    if (child && GetWindowRect(child, &r)) {
      editorW_ = r.right - r.left;
      editorH_ = r.bottom - r.top;
    }
  }
}

void Win32Gui::closeEditor() {
  if (!editorOpen_) return;
  editorOpen_ = false;
  if (auto h = plugin_.hosted())
    if (!h->dead()) h->editorClose();
  destroyLeftovers(editorHost_);
}

void Win32Gui::beforeUnload(HostedPlugin* p) {
  if (editorOpen_ && plugin_.hosted() == p) {
    editorOpen_ = false;
    if (p && !p->dead()) p->editorClose();
    if (editorHost_) destroyLeftovers(editorHost_);
  }
  editorW_ = editorH_ = 0;
}

void Win32Gui::onEditorResize(int width, int height) {
  editorW_ = width;
  editorH_ = height;
  if (page_now_ == Page::Editor) {
    layout();
    requestSize();
  }
}

void Win32Gui::onPluginChanged() {
  if (plugin_.hosted()) browserForced_ = false;
  refreshPrograms();
  genValues_.clear();
  genTexts_.clear();
  applyPage();
}

void Win32Gui::onCrash() {
  editorOpen_ = false;  // never call into it again
  applyPage();
}

void Win32Gui::onParamsChanged() {
  genTexts_.clear();
  refreshPrograms();
  if (generic_) InvalidateRect(generic_, nullptr, FALSE);
}

void Win32Gui::onParamValue(uint32_t) {
  if (generic_ && page_now_ == Page::Generic) InvalidateRect(generic_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------
// Browser actions

void Win32Gui::refreshFolders() {
  auto& st = Shared::get().settings;
  if (st.folders.empty()) {
    st.folders = defaultVstFolders();
    if (!st.folders.empty()) st.save();
  }
  SendMessageW(lstFolders_, LB_RESETCONTENT, 0, 0);
  for (auto& f : st.folders) SendMessageW(lstFolders_, LB_ADDSTRING, 0, (LPARAM)W(f).c_str());
}

void Win32Gui::addFolder() {
  HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose a folder with VST2 plugins (.dll)");
    if (SUCCEEDED(dlg->Show(GetAncestor(root_, GA_ROOT)))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          auto& st = Shared::get().settings;
          std::string p = narrow(path);
          bool dup = false;
          for (auto& f : st.folders)
            if (foldCase(f) == foldCase(p)) dup = true;
          if (!dup) {
            st.folders.push_back(p);
            st.save();
            refreshFolders();
            startScan(false, false);
          }
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(hrInit)) CoUninitialize();
}

void Win32Gui::removeFolder() {
  int sel = (int)SendMessageW(lstFolders_, LB_GETCURSEL, 0, 0);
  auto& st = Shared::get().settings;
  if (sel < 0 || sel >= (int)st.folders.size()) return;
  st.folders.erase(st.folders.begin() + sel);
  st.save();
  refreshFolders();
  Shared::get().db.prune(st.folders);
  Shared::get().db.save();
  refreshList();
}

void Win32Gui::startScan(bool rescanAll, bool retryFailed, const std::vector<std::string>& onlyFiles) {
  auto& sh = Shared::get();
  if (sh.scanner.running()) return;
  if (sh.settings.folders.empty() && onlyFiles.empty()) {
    MessageBoxW(GetAncestor(root_, GA_ROOT), L"Add a folder with VST2 plugins first.", L"YeOldeVST",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  Scanner::Options o;
  o.folders = sh.settings.folders;
  o.onlyFiles = onlyFiles;
  o.rescanAll = rescanAll;
  o.retryFailed = retryFailed;
  o.testEditors = sh.settings.testEditors;
  o.hiddenDesktop = sh.settings.hiddenDesktop;
  o.timeoutMs = sh.settings.timeoutSec * 1000;
  o.parallel = sh.settings.parallel;
  sh.scanner.start(o);
  wasScanning_ = true;
  refreshScanStatus();
}

void Win32Gui::refreshList() {
  auto all = Shared::get().db.all();
  int filter = (int)SendMessageW(cmbFilter_, CB_GETCURSEL, 0, 0);
  wchar_t sbuf[256] = {};
  GetWindowTextW(edtSearch_, sbuf, 256);
  std::string search = foldCase(trim(narrow(sbuf)));
  // Remember the selected row so it stays selected after the rebuild (scans refresh often).
  std::string selId;
  {
    int sel = ListView_GetNextItem(lv_, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < (int)rows_.size()) {
      auto& g = groups_[rows_[sel].group];
      selId = g.key + "#" + (rows_[sel].member < 0 ? std::string() : g.recs[rows_[sel].member].key());
    }
  }
  std::vector<PluginRecord> shown;
  for (auto& r : all) {
    if (r.status == ScanStatus::Shell) continue;
    bool other = r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported;
    switch (filter) {
      case F_ALL: if (other) continue; break;
      case F_WORKING: if (!statusLoadable(r.status)) continue; break;
      case F_PROBLEMS: if (!statusAngry(r.status) && r.status != ScanStatus::Warning && r.status != ScanStatus::NoHelper) continue; break;
      case F_INSTRUMENTS: if (other || !r.isInstrument()) continue; break;
      case F_EFFECTS: if (other || r.isInstrument() || r.status == ScanStatus::LoadFailed) continue; break;
      case F_32BIT: if (other || r.arch != "x86") continue; break;
      case F_OTHER: if (!other) continue; break;
    }
    if (!search.empty()) {
      std::string hay = foldCase(r.displayName() + " " + r.vendor + " " + r.path);
      if (hay.find(search) == std::string::npos) continue;
    }
    shown.push_back(r);
  }

  // Group copies of the same plugin (same name + vendor). Files that aren't plugins stay alone.
  groups_.clear();
  std::map<std::string, size_t> index;
  for (auto& r : shown) {
    bool other = r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported;
    std::string key = other ? "file|" + foldCase(r.path) + "|" + std::to_string(r.shellId)
                            : foldCase(r.displayName()) + "|" + foldCase(r.vendor);
    auto it = index.find(key);
    if (it == index.end()) {
      index[key] = groups_.size();
      groups_.push_back({key, {r}, 0});
    } else {
      groups_[it->second].recs.push_back(r);
    }
  }
  // The copy "Load" uses: working beats warning beats the rest; 64-bit beats 32-bit.
  auto rank = [](const PluginRecord& r) {
    int s = r.status == ScanStatus::Ok ? 0 : r.status == ScanStatus::Warning ? 2 : 10 + (int)r.status;
    return s + (r.arch == "x64" ? 0 : 1);
  };
  for (auto& g : groups_) {
    std::sort(g.recs.begin(), g.recs.end(),
              [](const PluginRecord& a, const PluginRecord& b) { return foldCase(a.path) < foldCase(b.path); });
    for (size_t i = 1; i < g.recs.size(); ++i)
      if (rank(g.recs[i]) < rank(g.recs[g.best])) g.best = (int)i;
  }

  auto key = [&](const Group& g) -> std::string {
    const PluginRecord& r = g.recs[g.best];
    switch (sortColumn_) {
      case 1: return foldCase(r.vendor);
      case 2: return r.isInstrument() ? "a" : "b";
      case 3: return r.arch;
      case 4: return std::to_string((int)r.status);
      case 5: return r.editor;
      case 6: return foldCase(r.detail);
      case 7: return foldCase(r.path);
      default: return foldCase(r.displayName());
    }
  };
  std::stable_sort(groups_.begin(), groups_.end(), [&](const Group& a, const Group& b) {
    auto ka = key(a), kb = key(b);
    if (ka == kb) return foldCase(a.recs[a.best].displayName()) < foldCase(b.recs[b.best].displayName());
    return sortAscending_ ? ka < kb : ka > kb;
  });

  rows_.clear();
  int newSel = -1;
  for (int g = 0; g < (int)groups_.size(); ++g) {
    auto rowId = [&](int m) {
      return groups_[g].key + "#" + (m < 0 ? std::string() : groups_[g].recs[m].key());
    };
    if (groups_[g].recs.size() == 1) {
      if (rowId(0) == selId) newSel = (int)rows_.size();
      rows_.push_back({g, 0});
      continue;
    }
    if (rowId(-1) == selId) newSel = (int)rows_.size();
    rows_.push_back({g, -1});
    if (expanded_.count(groups_[g].key))
      for (int m = 0; m < (int)groups_[g].recs.size(); ++m) {
        if (rowId(m) == selId) newSel = (int)rows_.size();
        rows_.push_back({g, m});
      }
  }
  ListView_SetItemCountEx(lv_, (int)rows_.size(), LVSICF_NOSCROLL);
  ListView_SetItemState(lv_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  if (newSel >= 0) ListView_SetItemState(lv_, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  InvalidateRect(lv_, nullptr, FALSE);
  if (page_) InvalidateRect(page_, &statusRect_, TRUE);
}

const PluginRecord* Win32Gui::rowRecord(int row) const {
  if (row < 0 || row >= (int)rows_.size()) return nullptr;
  auto& g = groups_[rows_[row].group];
  return &g.recs[rows_[row].member < 0 ? g.best : rows_[row].member];
}

bool Win32Gui::rowIsGroup(int row) const { return row >= 0 && row < (int)rows_.size() && rows_[row].member < 0; }

void Win32Gui::toggleGroup(int row) {
  if (!rowIsGroup(row)) return;
  auto& key = groups_[rows_[row].group].key;
  if (!expanded_.erase(key)) expanded_.insert(key);
  ListView_SetItemState(lv_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetItemState(lv_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  refreshList();
}

static std::wstring editorLabel(const PluginRecord& r) {
  if (r.editor == "ok") return L"OK";
  if (r.editor == "none" || (!r.hasEditor && statusLoadable(r.status))) return L"—";
  if (r.editor == "crashed") return L"Crashed";
  if (r.editor == "hangs") return L"Hangs";
  if (r.editor == "failed") return L"Failed";
  return (r.hasEditor && statusLoadable(r.status)) ? L"not tested" : L"";
}

std::wstring Win32Gui::cellText(int row, int col) const {
  if (row < 0 || row >= (int)rows_.size()) return {};
  const Group& g = groups_[rows_[row].group];
  const bool header = rows_[row].member < 0;
  const bool child = !header && g.recs.size() > 1;
  const PluginRecord& rec = *rowRecord(row);
  const bool other = rec.status == ScanStatus::NotPlugin || rec.status == ScanStatus::LoadFailed ||
                     rec.status == ScanStatus::Unsupported;
  switch (col) {
    case 0:
      if (header) return (expanded_.count(g.key) ? L"▾  " : L"▸  ") + W(rec.displayName());
      if (child) return L"        " + W(fileNameOf(rec.path));
      return W(rec.displayName());
    case 1: return child ? std::wstring() : W(rec.vendor);
    case 2: return other || child ? std::wstring() : (rec.isInstrument() ? L"Instrument" : L"Effect");
    case 3: {
      if (header) {
        bool b32 = false, b64 = false;
        for (auto& r : g.recs) (r.arch == "x86" ? b32 : b64) = true;
        return b32 && b64 ? L"32+64" : b32 ? L"32" : L"64";
      }
      return rec.arch == "x86" ? L"32" : rec.arch == "x64" ? L"64" : W(rec.arch);
    }
    case 4: return statusLabel(rec.status);
    case 5: {
      if (!header) return editorLabel(rec);
      std::wstring first;
      bool same = true, any = false;
      for (auto& r : g.recs) {
        if (!statusLoadable(r.status)) continue;
        auto l = editorLabel(r);
        if (!any) first = l;
        else if (l != first) same = false;
        any = true;
      }
      return any ? (same ? first : L"mixed") : std::wstring();
    }
    case 6: {
      if (!header) return W(rec.detail);
      int ok = 0, bad = 0;
      for (auto& r : g.recs) {
        if (statusLoadable(r.status)) ++ok;
        else ++bad;
      }
      wchar_t b[128];
      swprintf(b, 128, L"%d files: %d work, %d with problems", (int)g.recs.size(), ok, bad);
      return b;
    }
    case 7: return header ? W(dirOf(rec.path)) : W(rec.path);
  }
  return {};
}

void Win32Gui::refreshPrograms() {
  if (!cmbProgram_) return;
  SendMessageW(cmbProgram_, CB_RESETCONTENT, 0, 0);
  auto h = plugin_.hosted();
  if (!h || h->dead() || h->info().numPrograms <= 1) return;
  auto names = h->programNames();
  for (size_t i = 0; i < names.size(); ++i) {
    std::wstring s = std::to_wstring(i + 1) + L": " + W(names[i]);
    SendMessageW(cmbProgram_, CB_ADDSTRING, 0, (LPARAM)s.c_str());
  }
  SendMessageW(cmbProgram_, CB_SETCURSEL, h->currentProgram(), 0);
}

void Win32Gui::refreshScanStatus() {
  lastProgress_ = Shared::get().scanner.progress();
  bool running = Shared::get().scanner.running();
  EnableWindow(btnStop_, running);
  EnableWindow(btnScan_, !running);
  EnableWindow(btnRescan_, !running);
  EnableWindow(btnRetry_, !running);
  if (page_) {
    RECT r = statusRect_;
    r.bottom = progressRect_.bottom;
    InvalidateRect(page_, &r, TRUE);
  }
}

void Win32Gui::loadSelected(bool sandboxed) {
  // A group row loads its best copy (working, preferably 64-bit); expand it to pick another.
  if (const PluginRecord* r = rowRecord(ListView_GetNextItem(lv_, -1, LVNI_SELECTED))) loadRecord(*r, sandboxed);
}

void Win32Gui::loadRecord(const PluginRecord& r, bool sandboxed) {
  HWND owner = GetAncestor(root_, GA_ROOT);
  if (r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported) {
    MessageBoxW(owner, W(r.displayName() + " can't be loaded: " + r.detail).c_str(), L"YeOldeVST",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  bool bridged = sandboxed || r.arch == "x86";
  if (statusAngry(r.status) || r.status == ScanStatus::Unknown) {
    std::string msg = r.displayName() + " had a problem when it was tested:\n\n" +
                      (r.detail.empty() ? std::string("it has not been tested yet") : r.detail) + "\n\n";
    msg += bridged ? "It will run in a separate process, so if it crashes your DAW keeps running. Load it anyway?"
                   : "Loading it straight into your DAW could crash the DAW. Load it sandboxed (in a separate "
                     "process) instead?\n\nYes = sandboxed, No = load directly anyway, Cancel = don't load";
    int res = MessageBoxW(owner, W(msg).c_str(), L"YeOldeVST",
                          (bridged ? MB_OKCANCEL : MB_YESNOCANCEL) | MB_ICONWARNING | MB_DEFBUTTON1);
    if (res == IDCANCEL) return;
    if (!bridged && res == IDYES) sandboxed = true;
  }
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  std::string err;
  if (!plugin_.loadVst(r, sandboxed, err)) {
    MessageBoxW(owner, W("Could not load " + r.displayName() + ":\n\n" + err).c_str(), L"YeOldeVST",
                MB_OK | MB_ICONERROR);
    return;
  }
  browserForced_ = false;
  applyPage();
}

void Win32Gui::showContextMenu(int x, int y) {
  int sel = ListView_GetNextItem(lv_, -1, LVNI_SELECTED);
  const PluginRecord* rp = rowRecord(sel);
  if (!rp) return;
  const PluginRecord r = *rp;
  const bool group = rowIsGroup(sel);
  const bool busy = Shared::get().scanner.running();
  HMENU m = CreatePopupMenu();
  std::wstring loadText = group ? L"Load best copy (" + W(fileNameOf(r.path)) + L")" : std::wstring(L"Load");
  AppendMenuW(m, MF_STRING, ID_CTX_LOAD, loadText.c_str());
  AppendMenuW(m, MF_STRING, ID_CTX_LOAD_SANDBOXED, L"Load sandboxed (separate process)");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  if (group) {
    bool open = expanded_.count(groups_[rows_[sel].group].key) != 0;
    AppendMenuW(m, MF_STRING, ID_CTX_TOGGLE, open ? L"Collapse" : L"Show all copies");
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), ID_CTX_RESCAN, L"Test all copies again");
  } else {
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), ID_CTX_RESCAN, L"Test again");
  }
  AppendMenuW(m, MF_STRING, ID_CTX_EXPLORE, L"Show in Explorer");
  AppendMenuW(m, MF_STRING, ID_CTX_COPY, L"Copy path");
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, root_, nullptr);
  DestroyMenu(m);
  switch (cmd) {
    case ID_CTX_LOAD: loadRecord(r, false); break;
    case ID_CTX_LOAD_SANDBOXED: loadRecord(r, true); break;
    case ID_CTX_TOGGLE: toggleGroup(sel); break;
    case ID_CTX_RESCAN: {
      std::vector<std::string> files;
      if (group)
        for (auto& c : groups_[rows_[sel].group].recs) files.push_back(c.path);
      else
        files.push_back(r.path);
      startScan(false, false, files);
      break;
    }
    case ID_CTX_EXPLORE: {
      std::wstring args = L"/select,\"" + W(r.path) + L"\"";
      ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      break;
    }
    case ID_CTX_COPY: {
      std::wstring p = W(r.path);
      if (OpenClipboard(root_)) {
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (p.size() + 1) * sizeof(wchar_t));
        if (g) {
          memcpy(GlobalLock(g), p.c_str(), (p.size() + 1) * sizeof(wchar_t));
          GlobalUnlock(g);
          SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
      }
      break;
    }
  }
}

void Win32Gui::onCommand(int id, int code, HWND) {
  auto& sh = Shared::get();
  switch (id) {
    case ID_BROWSE:
      if (currentPage() == Page::Browser && plugin_.hosted()) {
        browserForced_ = false;
      } else {
        browserForced_ = true;
        sh.db.reloadIfChanged();
        refreshList();
      }
      applyPage();
      break;
    case ID_PROGRAM:
      if (code == CBN_SELCHANGE) {
        int sel = (int)SendMessageW(cmbProgram_, CB_GETCURSEL, 0, 0);
        if (sel >= 0) plugin_.setProgram(sel);
        genTexts_.clear();
        InvalidateRect(generic_, nullptr, FALSE);
      }
      break;
    case ID_VIEW:
      plugin_.setGenericView(!plugin_.genericView());
      applyPage();
      break;
    case ID_UNLOAD:
      plugin_.unloadVst();
      browserForced_ = true;
      applyPage();
      break;
    case ID_RELOAD: {
      std::string err;
      if (!plugin_.reload(err))
        MessageBoxW(GetAncestor(root_, GA_ROOT), W("Reload failed:\n\n" + err).c_str(), L"YeOldeVST",
                    MB_OK | MB_ICONERROR);
      applyPage();
      break;
    }
    case ID_ADD_FOLDER: addFolder(); break;
    case ID_REMOVE_FOLDER: removeFolder(); break;
    case ID_SCAN: startScan(false, false); break;
    case ID_RESCAN: startScan(true, false); break;
    case ID_RETRY: startScan(false, true); break;
    case ID_STOP: sh.scanner.cancel(); break;
    case ID_TGL_EDITORS:
    case ID_TGL_HIDDEN: {
      HWND b = id == ID_TGL_EDITORS ? tglEditors_ : tglHidden_;
      toggles_[b] = !toggles_[b];
      sh.settings.testEditors = toggles_[tglEditors_];
      sh.settings.hiddenDesktop = toggles_[tglHidden_];
      sh.settings.save();
      InvalidateRect(b, nullptr, FALSE);
      break;
    }
    case ID_SEARCH:
      if (code == EN_CHANGE) refreshList();
      break;
    case ID_FILTER:
      if (code == CBN_SELCHANGE) refreshList();
      break;
    case ID_LOAD: loadSelected(false); break;
    case ID_LOAD_SANDBOXED: loadSelected(true); break;
  }
}

void Win32Gui::onTimer() {
  ++timerTicks_;
  auto h = plugin_.hosted();
  if (editorOpen_ && h && !h->dead()) h->editorIdle();
  if (h && h->dead() && page_now_ != Page::Message && !browserForced_) onCrash();

  if (timerTicks_ % 6 == 0) {  // ~180 ms
    bool running = Shared::get().scanner.running();
    if (running || wasScanning_) {
      refreshScanStatus();
      if (timerTicks_ % 30 == 0 || !running) refreshList();
      if (!running) wasScanning_ = false;
    }
    if (page_now_ == Page::Generic && generic_) InvalidateRect(generic_, nullptr, FALSE);
  }
  if (timerTicks_ % 70 == 0 && page_now_ == Page::Browser) {  // ~2 s: pick up scans by other instances
    if (Shared::get().db.reloadIfChanged()) refreshList();
    if (Shared::get().scanner.running() != !IsWindowEnabled(btnScan_)) refreshScanStatus();
  }
}

// ---------------------------------------------------------------------------------------------
// Painting

void Win32Gui::drawButton(const DRAWITEMSTRUCT* dis) {
  HDC dc = dis->hDC;
  RECT r = dis->rcItem;
  bool pressed = dis->itemState & ODS_SELECTED;
  bool disabled = dis->itemState & ODS_DISABLED;
  bool focus = dis->itemState & ODS_FOCUS;
  wchar_t text[128];
  GetWindowTextW(dis->hwndItem, text, 128);
  fillRect(dc, r, GetParent(dis->hwndItem) == root_ ? kPanel : kBg);

  auto tg = toggles_.find(dis->hwndItem);
  if (tg != toggles_.end()) {
    // Toggle: checkbox square + label.
    int box = S(14);
    RECT b{r.left + S(2), (r.top + r.bottom - box) / 2, r.left + S(2) + box, (r.top + r.bottom + box) / 2};
    roundRect(dc, b, tg->second ? kAccent : kCtrl, tg->second ? kAccent : kBorder, S(4));
    if (tg->second) {
      HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), kAccentText);
      HGDIOBJ op = SelectObject(dc, pen);
      MoveToEx(dc, b.left + box * 3 / 14, b.top + box * 7 / 14, nullptr);
      LineTo(dc, b.left + box * 6 / 14, b.top + box * 10 / 14);
      LineTo(dc, b.left + box * 11 / 14, b.top + box * 4 / 14);
      SelectObject(dc, op);
      DeleteObject(pen);
    }
    RECT t = r;
    t.left = b.right + S(6);
    drawText(dc, text, t, disabled ? kGrey : kText, font_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return;
  }

  int id = GetDlgCtrlID(dis->hwndItem);
  bool primary = id == ID_LOAD || id == ID_SCAN || id == ID_RELOAD;
  COLORREF fill = primary ? kAccent : kCtrl;
  if (pressed) fill = primary ? RGB(0xB8, 0x8A, 0x35) : kCtrlHot;
  if (disabled) fill = kCtrl;
  COLORREF border = focus ? kAccent : (primary && !disabled ? kAccent : kBorder);
  roundRect(dc, r, fill, border, S(6));
  COLORREF tc = disabled ? kGrey : (primary ? kAccentText : kText);
  drawText(dc, text, r, tc, primary ? fontBold_ : font_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Win32Gui::paintRoot(HDC dc, const RECT& rc) {
  int tb = S(kToolbarH);
  RECT bar{0, 0, rc.right, tb};
  fillRect(dc, bar, kPanel);
  RECT line{0, tb - 1, rc.right, tb};
  fillRect(dc, line, kBorder);
  RECT body{0, tb, rc.right, rc.bottom};
  fillRect(dc, body, kBg);

  // Title area between the Plugins button and the right-hand controls.
  int left = S(8) + S(96) + S(12);
  int right = rc.right - S(8);
  for (HWND c : {cmbProgram_, btnView_, btnUnload_}) {
    if (c && IsWindowVisible(c)) {
      RECT cr;
      GetWindowRect(c, &cr);
      MapWindowPoints(nullptr, root_, (POINT*)&cr, 2);
      right = std::min<int>(right, cr.left - S(8));
    }
  }
  auto h = plugin_.hosted();
  std::wstring title, sub;
  COLORREF subColor = kDim;
  if (page_now_ == Page::Browser) {
    title = L"YeOldeVST";
    sub = h ? L"Pick a plugin to replace " + W(plugin_.loadedRecord().displayName())
            : (plugin_.isInstrument() ? L"Pick a VST instrument" : L"Pick a VST effect");
  } else if (h) {
    title = W(plugin_.loadedRecord().displayName());
    std::string vendor = h->info().vendor.empty() ? plugin_.loadedRecord().vendor : h->info().vendor;
    sub = W(vendor);
    if (h->sandboxed()) {
      sub += vendor.empty() ? L"" : L"  \u2022  ";
      sub += plugin_.loadedRecord().arch == "x86" ? L"32-bit, bridged" : L"sandboxed";
    }
    if (h->dead()) {
      sub = L"crashed";
      subColor = kBad;
    }
  } else {
    title = L"YeOldeVST";
    sub = L"missing plugin";
    subColor = kWarn;
  }
  if (right > left) {
    RECT t{left, 0, right, tb / 2 + S(3)};
    drawText(dc, title, t, kText, fontTitle_, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT s{left, tb / 2 + S(2), right, tb};
    drawText(dc, sub, s, subColor, fontSmall_, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  if (page_now_ == Page::Message) {
    std::wstring msg;
    if (h && h->dead()) {
      msg = W(plugin_.loadedRecord().displayName()) + L" crashed and has been switched off. Your DAW is fine; the "
            L"track is muted until you reload it.\n\n" + W(h->deathReason());
    } else {
      msg = W(plugin_.missingDescription());
    }
    RECT m{S(30), tb + S(30), rc.right - S(30), tb + (rc.bottom - tb) / 2 + S(20)};
    drawText(dc, msg, m, kText, font_, DT_CENTER | DT_WORDBREAK | DT_BOTTOM);
  }
}

void Win32Gui::paintPage(HDC dc, const RECT& rc) {
  fillRect(dc, rc, kBg);
  drawText(dc, L"VST2 PLUGIN FOLDERS", foldersLabelRect_, kDim, fontSmall_, DT_LEFT | DT_TOP | DT_SINGLELINE);

  // status line + progress
  auto& p = lastProgress_;
  std::wstring status;
  if (p.running) {
    wchar_t b[256];
    swprintf(b, 256, L"Testing %d of %d   \u2022   %d OK   \u2022   %d with problems", p.done, p.total, p.ok, p.angry);
    status = b;
    if (!p.current.empty()) status += L"   \u2022   " + W(fileNameOf(p.current.front()));
  } else if (!p.message.empty()) {
    status = W(p.message);
  } else {
    int ok = 0, bad = 0;
    for (auto& r : Shared::get().db.all()) {
      if (statusLoadable(r.status)) ++ok;
      if (statusAngry(r.status)) ++bad;
    }
    wchar_t b[200];
    swprintf(b, 200, L"%d plugins work, %d have problems. Double-click a plugin to load it.", ok, bad);
    status = b;
  }
  drawText(dc, status, statusRect_, p.running ? kText : kDim, font_, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                                                         DT_END_ELLIPSIS);
  if (p.running && p.total > 0) {
    fillRect(dc, progressRect_, kCtrl);
    RECT f = progressRect_;
    f.right = f.left + (int)((f.right - f.left) * (double)p.done / p.total);
    fillRect(dc, f, kAccent);
  }
}

LRESULT CALLBACK Win32Gui::listSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
  if (m == WM_NOTIFY) {
    auto nm = reinterpret_cast<NMHDR*>(l);
    if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(h))
      return reinterpret_cast<Win32Gui*>(ref)->headerCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(l));
  } else if (m == WM_NCDESTROY) {
    RemoveWindowSubclass(h, listSubclass, 1);
  }
  return DefSubclassProc(h, m, w, l);
}

LRESULT Win32Gui::headerCustomDraw(NMCUSTOMDRAW* cd) {
  if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    bool hot = (cd->uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
    fillRect(dc, r, hot ? kCtrlHot : kCtrl);
    RECT sep{r.right - 1, r.top + S(4), r.right, r.bottom - S(4)};
    fillRect(dc, sep, kBorder);
    wchar_t text[64] = {};
    HDITEMW item{};
    item.mask = HDI_TEXT;
    item.pszText = text;
    item.cchTextMax = 64;
    Header_GetItem(cd->hdr.hwndFrom, (int)cd->dwItemSpec, &item);
    std::wstring t = text;
    if ((int)cd->dwItemSpec == sortColumn_) t += sortAscending_ ? L"  \u25B4" : L"  \u25BE";
    r.left += S(6);
    drawText(dc, t, r, kText, fontBold_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return CDRF_SKIPDEFAULT;
  }
  return CDRF_DODEFAULT;
}

LRESULT Win32Gui::listCustomDraw(NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      int row = (int)cd->nmcd.dwItemSpec;
      bool selected = ListView_GetItemState(lv_, row, LVIS_SELECTED) & LVIS_SELECTED;
      // Suppress the theme's own selection highlight; we paint it ourselves.
      cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
      cd->clrTextBk = selected ? kSelect : (row % 2 ? kRowAlt : kPanel);
      cd->clrText = kText;
      return CDRF_NOTIFYSUBITEMDRAW;
    }
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
      int row = (int)cd->nmcd.dwItemSpec;
      const PluginRecord* rp = rowRecord(row);
      if (!rp) return CDRF_DODEFAULT;
      auto& r = *rp;
      const bool group = rowIsGroup(row);
      SelectObject(cd->nmcd.hdc, group ? fontBold_ : font_);
      cd->clrText = kText;
      if (cd->iSubItem == 4) {
        cd->clrText = statusColor(r.status);
      } else if (cd->iSubItem == 5) {
        auto t = cellText(row, 5);
        cd->clrText = t == L"OK" ? kOk : (t == L"Crashed" || t == L"Hangs" || t == L"Failed") ? kBad
                      : t == L"mixed"                                                       ? kWarn
                                                                                            : kDim;
      } else if (cd->iSubItem == 3 && cellText(row, 3) != L"64") {
        cd->clrText = kBlue;
      } else if (cd->iSubItem >= 6 || cd->iSubItem == 1 || cd->iSubItem == 2) {
        cd->clrText = kDim;
      }
      if (r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported) cd->clrText = kGrey;
      return CDRF_NEWFONT;
    }
  }
  return CDRF_DODEFAULT;
}

// ---------------------------------------------------------------------------------------------
// Generic editor

int Win32Gui::genericRowAt(int y) const {
  int row = (y + genScroll_) / S(kGenericRowH);
  int n = (int)plugin_.paramNames().size();
  return (row >= 0 && row < n) ? row : -1;
}

void Win32Gui::genericSetFromX(int row, int x) {
  RECT rc;
  GetClientRect(generic_, &rc);
  int nameW = rc.right * 38 / 100;
  int valueW = rc.right * 20 / 100;
  int barL = nameW + S(8), barR = rc.right - valueW - S(8);
  if (barR <= barL) return;
  double v = std::clamp((double)(x - barL) / (barR - barL), 0.0, 1.0);
  plugin_.editParameter((uint32_t)row, v);
  if (row < (int)genTexts_.size()) genTexts_[row].clear();
  InvalidateRect(generic_, nullptr, FALSE);
}

void Win32Gui::genericUpdateScroll() {
  if (!generic_) return;
  RECT rc;
  GetClientRect(generic_, &rc);
  int total = (int)plugin_.paramNames().size() * S(kGenericRowH);
  SCROLLINFO si{sizeof si};
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = std::max(0, total - 1);
  si.nPage = (UINT)rc.bottom;
  genScroll_ = std::clamp(genScroll_, 0, std::max(0, total - (int)rc.bottom));
  si.nPos = genScroll_;
  SetScrollInfo(generic_, SB_VERT, &si, TRUE);
}

void Win32Gui::genericScrollTo(int pos) {
  RECT rc;
  GetClientRect(generic_, &rc);
  int total = (int)plugin_.paramNames().size() * S(kGenericRowH);
  genScroll_ = std::clamp(pos, 0, std::max(0, total - (int)rc.bottom));
  genericUpdateScroll();
  InvalidateRect(generic_, nullptr, FALSE);
}

void Win32Gui::paintGeneric(HDC dc, const RECT& rc) {
  fillRect(dc, rc, kBg);
  auto h = plugin_.hosted();
  auto& names = plugin_.paramNames();
  if (!h || h->dead()) return;
  if (names.empty()) {
    drawText(dc, L"This plugin has no parameters to show.", rc, kDim, font_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  int rowH = S(kGenericRowH);
  int first = genScroll_ / rowH;
  int last = std::min<int>((int)names.size() - 1, (genScroll_ + rc.bottom) / rowH);
  if (genValues_.size() != names.size()) genValues_.assign(names.size(), -1.f);
  if (genTexts_.size() != names.size()) genTexts_.assign(names.size(), std::string());
  int nameW = rc.right * 38 / 100;
  int valueW = rc.right * 20 / 100;
  for (int i = first; i <= last; ++i) {
    int y = i * rowH - genScroll_;
    RECT row{0, y, rc.right, y + rowH};
    fillRect(dc, row, i % 2 ? kRowAlt : kBg);
    float v = std::clamp(h->getParameter(i), 0.f, 1.f);
    if (v != genValues_[i] || genTexts_[i].empty()) {
      genValues_[i] = v;
      genTexts_[i] = h->paramDisplay(i);
      if (genTexts_[i].empty()) {
        char b[32];
        snprintf(b, sizeof b, "%.1f%%", v * 100.0);
        genTexts_[i] = b;
      }
    }
    RECT nr{S(10), y, nameW, y + rowH};
    drawText(dc, W(names[i]), nr, i == genDragRow_ ? kAccent : kText, font_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int barL = nameW + S(8), barR = rc.right - valueW - S(8);
    RECT track{barL, y + rowH / 2 - S(3), barR, y + rowH / 2 + S(3)};
    roundRect(dc, track, kCtrl, kCtrl, S(6));
    RECT fill = track;
    fill.right = barL + (int)((barR - barL) * v);
    if (fill.right > fill.left + 1) roundRect(dc, fill, kAccent, kAccent, S(6));
    int kx = fill.right;
    RECT knob{kx - S(5), y + rowH / 2 - S(7), kx + S(5), y + rowH / 2 + S(7)};
    roundRect(dc, knob, kText, kText, S(4));
    RECT vr{rc.right - valueW, y, rc.right - S(10), y + rowH};
    drawText(dc, W(genTexts_[i]), vr, kDim, font_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}

// ---------------------------------------------------------------------------------------------
// Window procedures

LRESULT CALLBACK Win32Gui::rootProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
  }
  auto self = reinterpret_cast<Win32Gui*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->onRoot(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Win32Gui::pageProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
  }
  auto self = reinterpret_cast<Win32Gui*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->onPage(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Win32Gui::genericProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
  }
  auto self = reinterpret_cast<Win32Gui*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->onGeneric(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// Messages both the root and the page handle the same way.
static bool commonColors(UINT m, WPARAM w, LRESULT& out, HBRUSH ctrl, HBRUSH panel) {
  switch (m) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      SetTextColor((HDC)w, kText);
      SetBkColor((HDC)w, kCtrl);
      out = (LRESULT)ctrl;
      return true;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      SetTextColor((HDC)w, kText);
      SetBkColor((HDC)w, kPanel);
      out = (LRESULT)panel;
      return true;
  }
  return false;
}

LRESULT Win32Gui::onRoot(HWND h, UINT m, WPARAM w, LPARAM l) {
  LRESULT r;
  if (commonColors(m, w, r, brCtrl_, brPanel_)) return r;
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      // Double buffer the toolbar / message area.
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ ob = SelectObject(mem, bmp);
      paintRoot(mem, rc);
      BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
             ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
      SelectObject(mem, ob);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_SIZE:
      layout();
      return 0;
    case WM_TIMER:
      if (w == kTimerId) onTimer();
      return 0;
    case WM_DRAWITEM:
      drawButton(reinterpret_cast<DRAWITEMSTRUCT*>(l));
      return TRUE;
    case WM_COMMAND:
      onCommand(LOWORD(w), HIWORD(w), (HWND)l);
      return 0;
    case WM_DPICHANGED_AFTERPARENT: {
      int d = (int)GetDpiForWindow(h);
      if (d > 0 && d != dpi_) {
        dpi_ = d;
        createFonts();
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
          SendMessageW(c, WM_SETFONT, (WPARAM)font_, TRUE);
        layout();
      }
      return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

LRESULT Win32Gui::onPage(HWND h, UINT m, WPARAM w, LPARAM l) {
  LRESULT r;
  if (commonColors(m, w, r, brCtrl_, brBg_)) return r;
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ ob = SelectObject(mem, bmp);
      paintPage(mem, rc);
      BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
             ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
      SelectObject(mem, ob);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_DRAWITEM:
      drawButton(reinterpret_cast<DRAWITEMSTRUCT*>(l));
      return TRUE;
    case WM_COMMAND:
      onCommand(LOWORD(w), HIWORD(w), (HWND)l);
      return 0;
    case WM_CONTEXTMENU:
      if ((HWND)w == lv_) {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (x == -1 && y == -1) {
          RECT rc;
          GetWindowRect(lv_, &rc);
          x = rc.left + S(20);
          y = rc.top + S(30);
        }
        showContextMenu(x, y);
        return 0;
      }
      break;
    case WM_NOTIFY: {
      auto nm = reinterpret_cast<NMHDR*>(l);
      if (nm->hwndFrom == ListView_GetHeader(lv_) && nm->code == NM_CUSTOMDRAW)
        return headerCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(l));
      if (nm->hwndFrom != lv_) break;
      switch (nm->code) {
        case NM_CUSTOMDRAW: return listCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(l));
        case LVN_GETDISPINFOW: {
          auto di = reinterpret_cast<NMLVDISPINFOW*>(l);
          int row = di->item.iItem;
          if (!(di->item.mask & LVIF_TEXT) || row < 0 || row >= (int)rows_.size()) return 0;
          wcsncpy_s(di->item.pszText, di->item.cchTextMax, cellText(row, di->item.iSubItem).c_str(), _TRUNCATE);
          return 0;
        }
        case LVN_GETINFOTIPW: {
          auto tip = reinterpret_cast<NMLVGETINFOTIPW*>(l);
          const PluginRecord* rp = rowRecord(tip->iItem);
          if (!rp) return 0;
          if (rowIsGroup(tip->iItem)) {
            // Group: one line per copy.
            auto& g = groups_[rows_[tip->iItem].group];
            std::string t = rp->displayName() + " (" + std::to_string(g.recs.size()) + " files)";
            int shown = 0;
            for (auto& c : g.recs) {
              if (++shown > 20) {
                t += "\n...";
                break;
              }
              t += "\n" + std::string(c.arch == "x86" ? "32" : "64") + "-bit  " + narrow(statusLabel(c.status)) +
                   "  " + fileNameOf(c.path);
            }
            t += "\nDouble-click to show all copies; Load picks " + fileNameOf(rp->path);
            wcsncpy_s(tip->pszText, tip->cchTextMax, W(t).c_str(), _TRUNCATE);
            return 0;
          }
          auto& rec = *rp;
          std::string t = rec.displayName();
          if (!rec.vendor.empty()) t += " by " + rec.vendor;
          t += "\n" + std::string(rec.arch == "x86" ? "32-bit" : "64-bit") + ", " + narrow(statusLabel(rec.status));
          if (!rec.detail.empty()) t += "\n" + rec.detail;
          for (size_t i = 1; i < rec.warnings.size(); ++i) t += "\n" + rec.warnings[i];
          if (rec.numParams) t += "\n" + std::to_string(rec.numParams) + " parameters";
          if (rec.numPrograms > 1) t += ", " + std::to_string(rec.numPrograms) + " presets";
          if (rec.cpuPercent > 0) {
            char b[64];
            snprintf(b, sizeof b, "\nCPU during test: %.1f%% of one core", rec.cpuPercent);
            t += b;
          }
          t += "\n" + rec.path;
          wcsncpy_s(tip->pszText, tip->cchTextMax, W(t).c_str(), _TRUNCATE);
          return 0;
        }
        case NM_CLICK: {
          // Clicking the arrow of a group row opens / closes it.
          auto ia = reinterpret_cast<NMITEMACTIVATE*>(l);
          if (rowIsGroup(ia->iItem) && ia->iSubItem == 0 && ia->ptAction.x < S(28)) toggleGroup(ia->iItem);
          return 0;
        }
        case NM_DBLCLK: {
          int row = ListView_GetNextItem(lv_, -1, LVNI_SELECTED);
          if (rowIsGroup(row)) toggleGroup(row);
          else loadSelected(false);
          return 0;
        }
        case LVN_KEYDOWN: {
          int row = ListView_GetNextItem(lv_, -1, LVNI_SELECTED);
          WORD vk = reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey;
          bool open = rowIsGroup(row) && expanded_.count(groups_[rows_[row].group].key);
          if (vk == VK_RETURN) loadSelected(false);
          else if (vk == VK_RIGHT && rowIsGroup(row) && !open) toggleGroup(row);
          else if (vk == VK_LEFT && rowIsGroup(row) && open) toggleGroup(row);
          return 0;
        }
        case LVN_COLUMNCLICK: {
          int col = reinterpret_cast<NMLISTVIEW*>(l)->iSubItem;
          if (col == sortColumn_) sortAscending_ = !sortAscending_;
          else {
            sortColumn_ = col;
            sortAscending_ = true;
          }
          refreshList();
          InvalidateRect(ListView_GetHeader(lv_), nullptr, TRUE);
          return 0;
        }
      }
      break;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

LRESULT Win32Gui::onGeneric(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ ob = SelectObject(mem, bmp);
      paintGeneric(mem, rc);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, ob);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_SIZE: genericUpdateScroll(); return 0;
    case WM_MOUSEWHEEL:
      genericScrollTo(genScroll_ - GET_WHEEL_DELTA_WPARAM(w) * S(kGenericRowH) * 3 / WHEEL_DELTA);
      return 0;
    case WM_VSCROLL: {
      SCROLLINFO si{sizeof si, SIF_ALL};
      GetScrollInfo(h, SB_VERT, &si);
      int pos = genScroll_;
      switch (LOWORD(w)) {
        case SB_LINEUP: pos -= S(kGenericRowH); break;
        case SB_LINEDOWN: pos += S(kGenericRowH); break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
      }
      genericScrollTo(pos);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      SetFocus(h);
      int row = genericRowAt(GET_Y_LPARAM(l));
      if (row < 0) return 0;
      genDragRow_ = row;
      SetCapture(h);
      plugin_.beginParameterEdit((uint32_t)row);
      genericSetFromX(row, GET_X_LPARAM(l));
      return 0;
    }
    case WM_MOUSEMOVE:
      if (genDragRow_ >= 0) genericSetFromX(genDragRow_, GET_X_LPARAM(l));
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (genDragRow_ >= 0) {
        plugin_.endParameterEdit((uint32_t)genDragRow_);
        genDragRow_ = -1;
        if (m == WM_LBUTTONUP) ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

}  // namespace yov
