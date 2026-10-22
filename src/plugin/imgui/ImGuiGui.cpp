// The plugin window drawn with Dear ImGui. Same pages, layout, texts and behaviour as the Win32
// GUI (plugin/win/Win32Gui.cpp); the list content comes from the same BrowserModel. The native
// parts (the window inside the host's, input, the editor's parent window) are a GuiWindow.
#include "plugin/imgui/ImGuiGui.h"

#include "common/Util.h"
#include "plugin/BrowserModel.h"
#include "plugin/Plugin.h"
#include "plugin/imgui/GuiWindow.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>  // ClearActiveID

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace yov {

namespace {

constexpr int kToolbarH = 40;
constexpr int kGenericRowH = 26;
#if defined(_WIN32)
// Segoe UI 9 / 11 / 8 pt at 96 dpi, like the Win32 GUI.
constexpr float kFontSize = 12.f;
constexpr float kFontSizeTitle = 14.67f;
constexpr float kFontSizeSmall = 10.67f;
#else
// The common Linux sans faces (DejaVu, Noto) run a little larger.
constexpr float kFontSize = 12.5f;
constexpr float kFontSizeTitle = 15.f;
constexpr float kFontSizeSmall = 10.5f;
#endif

ImU32 col(uint32_t rgb, float alpha = 1.f) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, (int)(alpha * 255.f));
}
ImVec4 colv(uint32_t rgb, float alpha = 1.f) {
  return ImVec4(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha);
}

std::string findFont(bool bold) {
  std::vector<std::string> c;
#if defined(_WIN32)
  std::string win;
  if (!getEnv("WINDIR", win)) win = "C:\\Windows";
  c.push_back(joinPath(joinPath(win, "Fonts"), bold ? "seguisb.ttf" : "segoeui.ttf"));
#elif defined(__APPLE__)
  c = bold ? std::vector<std::string>{"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"}
           : std::vector<std::string>{"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"};
#else
  const char* dirs[] = {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/", "/usr/share/fonts/dejavu/",
                        "/usr/share/fonts/dejavu-sans-fonts/", "/usr/local/share/fonts/"};
  for (auto d : dirs) c.push_back(std::string(d) + (bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf"));
  const char* noto[] = {"/usr/share/fonts/truetype/noto/", "/usr/share/fonts/noto/", "/usr/share/fonts/google-noto/"};
  for (auto d : noto) c.push_back(std::string(d) + (bold ? "NotoSans-Bold.ttf" : "NotoSans-Regular.ttf"));
  const char* lib[] = {"/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/liberation/"};
  for (auto d : lib) c.push_back(std::string(d) + (bold ? "LiberationSans-Bold.ttf" : "LiberationSans-Regular.ttf"));
#endif
  for (auto& p : c)
    if (fileExists(p)) return p;
  return {};
}

// A face with the arrows and bullets (U+25B8 ...) the UI face may lack, merged in as fallback.
std::string findSymbolFont() {
  std::vector<std::string> c;
#if defined(_WIN32)
  std::string win;
  if (!getEnv("WINDIR", win)) win = "C:\\Windows";
  c.push_back(joinPath(joinPath(win, "Fonts"), "seguisym.ttf"));
#elif defined(__APPLE__)
  c = {"/System/Library/Fonts/Apple Symbols.ttf"};
#else
  for (auto d : {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/", "/usr/share/fonts/dejavu/"})
    c.push_back(std::string(d) + "DejaVuSans.ttf");
#endif
  for (auto& p : c)
    if (fileExists(p)) return p;
  return {};
}

// Text cut to fit `width`, with "..." when it does not.
std::string ellipsize(const std::string& s, float width) {
  if (width <= 0) return {};
  if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
  const char* dots = "...";
  float avail = width - ImGui::CalcTextSize(dots).x;
  // Possible cut points: the ends of UTF-8 characters.
  std::vector<size_t> cuts;
  for (size_t i = 1; i <= s.size(); ++i)
    if (i == s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) cuts.push_back(i);
  int lo = -1, hi = (int)cuts.size() - 1;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (ImGui::CalcTextSize(s.data(), s.data() + cuts[mid]).x <= avail) lo = mid;
    else hi = mid - 1;
  }
  return (lo >= 0 ? s.substr(0, cuts[lo]) : std::string()) + dots;
}

// Splits text into lines that fit `width` (words kept whole where possible).
std::vector<std::string> wrapLines(const std::string& text, float width) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    std::string line;
    size_t i = 0;
    while (i <= para.size()) {
      size_t sp = para.find(' ', i);
      std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
      std::string trial = line.empty() ? word : line + " " + word;
      if (!line.empty() && ImGui::CalcTextSize(trial.c_str()).x > width) {
        out.push_back(line);
        line = word;
      } else {
        line = trial;
      }
      if (sp == std::string::npos) break;
      i = sp + 1;
    }
    out.push_back(line);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out;
}

class ImGuiGui final : public Gui {
 public:
  explicit ImGuiGui(Plugin& p);
  ~ImGuiGui() override;

  bool setParent(const clap_window_t* window) override;
  void getSize(uint32_t& w, uint32_t& h) const override {
    w = width_;
    h = height_;
  }
  void adjustSize(uint32_t& w, uint32_t& h) const override;
  bool setSize(uint32_t w, uint32_t h) override;
  void show() override {
    if (win_) win_->setVisible(true);
  }
  void hide() override {
    if (win_) win_->setVisible(false);
  }

  void onPluginChanged() override;
  void onCrash() override;
  void onEditorResize(int width, int height) override;
  void onParamsChanged() override;
  void onParamValue(uint32_t) override {
    if (pageNow_ == Page::Generic) dirty_ = true;
  }
  void beforeUnload(HostedPlugin* p) override;
  void onTimer(clap_id id) override {
    if (win_) win_->onHostTimer(id);
  }
  void onFd(int fd, clap_posix_fd_flags_t flags) override {
    if (win_) win_->onHostFd(fd, flags);
  }

 private:
  enum class Page { Browser, Editor, Generic, Message };

  int S(float v) const { return (int)std::lround(v * scale_); }
  Page currentPage() const;
  void applyPage();
  void preferredSize(uint32_t& w, uint32_t& h) const;
  void requestSize();

  void tick();
  void periodic();
  void renderFrame();
  void setupStyle();

  // Pages
  void drawToolbar();
  void drawBrowser();
  void drawPluginList(float x, float y, float w, float h);
  void drawGeneric();
  void drawMessage();
  void drawDialogs();
  void drawFolderPicker();

  // Widgets
  bool button(const char* id, const char* label, float x, float y, float w, float h, bool primary,
              bool enabled = true, const char* tip = nullptr);
  bool toggle(const char* id, const char* label, bool& value, float x, float y, float w, float h,
              const char* tip = nullptr);

  // Actions
  void refreshFolders();
  void addFolder();
  void addFolderPath(const std::string& p);
  void removeFolder();
  void startScan(bool rescanAll, bool retryFailed, const std::vector<std::string>& onlyFiles = {});
  void refreshList();
  void refreshPrograms();
  void refreshScanStatus();
  void loadSelected(bool sandboxed);
  void loadRecord(const PluginRecord& r, bool sandboxed);
  void doLoad(const PluginRecord& r, bool sandboxed);
  void openEditor();
  void closeEditor();
  void placeEditor();
  void selectRow(int row, bool scroll);

  // Dialogs: kind 0 info, 1 warning, 2 error; buttons 0 OK, 1 OK/Cancel, 2 Yes/No/Cancel. The
  // callback gets 0 OK/Yes, 1 No, 2 Cancel.
  void message(const std::string& text, int kind, int buttons = 0, std::function<void(int)> done = {});

  Plugin& plugin_;
  std::unique_ptr<GuiWindow> win_;
  ImGuiContext* ctx_ = nullptr;
  ImFont* font_ = nullptr;
  ImFont* fontBold_ = nullptr;
  bool glReady_ = false;
  float scale_ = 1.f;
  uint32_t width_ = 0, height_ = 0;

  bool browserForced_ = true;
  Page pageNow_ = Page::Browser;
  bool editorOpen_ = false;
  int editorW_ = 0, editorH_ = 0;

  // browser
  BrowserModel model_;
  int selRow_ = -1;
  int scrollToRow_ = -1;
  int ctxRow_ = -1;
  int filter_ = F_ALL;
  char search_[256] = {};
  int folderSel_ = -1;
  bool testEditors_ = true, hiddenDesktop_ = true;
  bool wasScanning_ = false;
  ScanProgress lastProgress_;
  std::vector<std::string> programNames_;
  int currentProgram_ = 0;

  // generic editor
  int genDragRow_ = -1;
  std::vector<float> genValues_;
  std::vector<std::string> genTexts_;

  // dialogs
  struct Dialog {
    std::string text;
    int kind = 0, buttons = 0;
    std::function<void(int)> done;
  };
  std::vector<Dialog> dialogs_;
  bool folderPicker_ = false;
  std::string pickDir_;
  std::vector<std::string> pickEntries_;
  int pickSel_ = -1;
  char pickPath_[1024] = {};

  // pacing
  int timerTicks_ = 0;
  uint64_t lastPeriodic_ = 0, lastFrame_ = 0;
  int framesLeft_ = 3;
  bool dirty_ = true;
};

ImGuiGui::ImGuiGui(Plugin& p) : plugin_(p) {
  browserForced_ = plugin_.hosted() == nullptr;
  auto& st = Shared::get().settings;
  testEditors_ = st.testEditors;
  hiddenDesktop_ = st.hiddenDesktop;
  filter_ = plugin_.isInstrument() ? F_INSTRUMENTS : F_EFFECTS;
  preferredSize(width_, height_);
}

ImGuiGui::~ImGuiGui() {
  closeEditor();
  if (ctx_) {
    ImGui::SetCurrentContext(ctx_);
    if (glReady_ && win_ && win_->beginGl(1, 1, palette::kBg)) {
      ImGui_ImplOpenGL3_Shutdown();
      win_->endGl(false);
    }
    ImGui::DestroyContext(ctx_);
  }
  win_.reset();
}

// ---------------------------------------------------------------------------------------------
// CLAP gui

bool ImGuiGui::setParent(const clap_window_t* window) {
  if (win_) return win_->attach(window, (int)width_, (int)height_);
  ctx_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(ctx_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.BackendPlatformName = "yeoldevst";
  win_ = GuiWindow::create(plugin_);
  if (!win_ || !win_->attach(window, (int)width_, (int)height_)) {
    win_.reset();
    return false;
  }
  scale_ = win_->scale();
  // Fonts: the system's UI face (sizes are chosen when drawing: dynamic font atlas).
  std::string reg = findFont(false), bold = findFont(true), sym = findSymbolFont();
  auto addFace = [&](const std::string& path) -> ImFont* {
    ImFont* f = path.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(path.c_str(), kFontSize);
    if (f && !sym.empty() && sym != path) {
      ImFontConfig merge;
      merge.MergeMode = true;
      io.Fonts->AddFontFromFileTTF(sym.c_str(), kFontSize, &merge);
    }
    return f;
  };
  font_ = addFace(reg);
  if (!font_) font_ = io.Fonts->AddFontDefault();
  fontBold_ = addFace(bold);
  if (!fontBold_) fontBold_ = font_;
  setupStyle();
  if (win_->beginGl((int)width_, (int)height_, palette::kBg)) {
    glReady_ = ImGui_ImplOpenGL3_Init(nullptr);
    win_->endGl(false);
  }
  if (!glReady_) logLine("gui: OpenGL renderer did not start");
  // The window's scale is known now: lay out for it.
  preferredSize(width_, height_);
  win_->resize((int)width_, (int)height_);
  plugin_.requestResize(width_, height_);
  refreshFolders();
  refreshList();
  refreshPrograms();
  applyPage();
  win_->startTicking([this] { tick(); });
  return true;
}

void ImGuiGui::setupStyle() {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  s.WindowPadding = ImVec2(8, 6);  // popups, menus, tooltips (the main window uses none)
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  s.PopupBorderSize = 1;
  s.FrameRounding = 3;
  s.PopupRounding = 4;
  s.GrabRounding = 3;
  s.ScrollbarRounding = 3;
  s.ScrollbarSize = 14;
  s.FramePadding = ImVec2(6, 4);
  s.ItemSpacing = ImVec2(6, 4);
  s.CellPadding = ImVec2(6, 2);
  s.FontSizeBase = kFontSize;
  s.HoverDelayNormal = 0.5f;
  s.HoverStationaryDelay = 0.3f;
  s.ScaleAllSizes(scale_);
  s.FontScaleDpi = scale_;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = colv(palette::kText);
  c[ImGuiCol_TextDisabled] = colv(palette::kGrey);
  c[ImGuiCol_WindowBg] = colv(palette::kBg);
  c[ImGuiCol_ChildBg] = colv(palette::kBg, 0);
  c[ImGuiCol_PopupBg] = colv(palette::kPanel);
  c[ImGuiCol_Border] = colv(palette::kBorder);
  c[ImGuiCol_FrameBg] = colv(palette::kCtrl);
  c[ImGuiCol_FrameBgHovered] = colv(palette::kCtrlHot);
  c[ImGuiCol_FrameBgActive] = colv(palette::kCtrlHot);
  c[ImGuiCol_Button] = colv(palette::kCtrl);
  c[ImGuiCol_ButtonHovered] = colv(palette::kCtrlHot);
  c[ImGuiCol_ButtonActive] = colv(palette::kCtrlHot);
  c[ImGuiCol_Header] = colv(palette::kSelect);
  c[ImGuiCol_HeaderHovered] = colv(palette::kCtrlHot, 0.55f);
  c[ImGuiCol_HeaderActive] = colv(palette::kSelect);
  c[ImGuiCol_CheckMark] = colv(palette::kAccent);
  c[ImGuiCol_SliderGrab] = colv(palette::kAccent);
  c[ImGuiCol_SliderGrabActive] = colv(palette::kAccent);
  c[ImGuiCol_ScrollbarBg] = colv(palette::kPanel);
  c[ImGuiCol_ScrollbarGrab] = colv(palette::kBorder);
  c[ImGuiCol_ScrollbarGrabHovered] = colv(palette::kCtrlHot);
  c[ImGuiCol_ScrollbarGrabActive] = colv(palette::kDim);
  c[ImGuiCol_Separator] = colv(palette::kBorder);
  c[ImGuiCol_TableHeaderBg] = colv(palette::kCtrl);
  c[ImGuiCol_TableBorderStrong] = colv(palette::kBorder);
  c[ImGuiCol_TableBorderLight] = colv(palette::kBorder);
  c[ImGuiCol_TableRowBg] = colv(palette::kPanel);
  c[ImGuiCol_TableRowBgAlt] = colv(palette::kRowAlt);
  c[ImGuiCol_TextSelectedBg] = colv(palette::kSelect);
  c[ImGuiCol_NavCursor] = colv(palette::kAccent);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
  c[ImGuiCol_ResizeGrip] = colv(palette::kBorder, 0);
  c[ImGuiCol_ResizeGripHovered] = colv(palette::kAccent, 0.6f);
  c[ImGuiCol_ResizeGripActive] = colv(palette::kAccent);
}

void ImGuiGui::adjustSize(uint32_t& w, uint32_t& h) const {
  if (pageNow_ == Page::Editor && editorW_ > 0) {
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

bool ImGuiGui::setSize(uint32_t w, uint32_t h) {
  width_ = w;
  height_ = h;
  if (win_) win_->resize((int)w, (int)h);
  placeEditor();
  dirty_ = true;
  return true;
}

void ImGuiGui::preferredSize(uint32_t& w, uint32_t& h) const {
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

void ImGuiGui::requestSize() {
  uint32_t w, h;
  preferredSize(w, h);
  if (w == width_ && h == height_) return;
  // Ask the host; most hosts resize the window and call setSize(). Lay out at the new size
  // either way, so the content is right even if the host does not follow.
  plugin_.requestResize(w, h);
  setSize(w, h);
}

ImGuiGui::Page ImGuiGui::currentPage() const {
  auto h = plugin_.hosted();
  if (browserForced_) return Page::Browser;
  if (!h) return plugin_.hasMissingPlugin() ? Page::Message : Page::Browser;
  if (h->dead()) return Page::Message;
  if (plugin_.genericView() || !h->info().hasEditor) return Page::Generic;
  return Page::Editor;
}

void ImGuiGui::applyPage() {
  Page p = currentPage();
  if (p != Page::Editor && editorOpen_) closeEditor();
  pageNow_ = p;
  if (p == Page::Editor && !editorOpen_) openEditor();
  if (p == Page::Generic) {
    genValues_.clear();
    genTexts_.clear();
  }
  requestSize();
  placeEditor();
  dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// Editor

void ImGuiGui::placeEditor() {
  if (!win_) return;
  bool show = pageNow_ == Page::Editor && editorOpen_;
  int tb = S(kToolbarH);
  int ew = editorW_ > 0 ? editorW_ : (int)width_;
  int eh = editorH_ > 0 ? editorH_ : (int)height_ - tb;
  win_->placeEditor(std::max(0, ((int)width_ - ew) / 2), tb, ew, eh, show || (pageNow_ == Page::Editor && !editorOpen_));
}

void ImGuiGui::openEditor() {
  auto h = plugin_.hosted();
  if (!h || editorOpen_ || !win_) return;
  int w = 0, hh = 0;
  h->editorSize(w, hh);
  if (w > 0 && hh > 0) {
    editorW_ = w;
    editorH_ = hh;
  }
  placeEditor();
  if (!h->editorOpen(win_->editorParent())) {
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
    int cw = 0, ch = 0;
    if (win_->editorChildSize(cw, ch)) {
      editorW_ = cw;
      editorH_ = ch;
    }
  }
  placeEditor();
}

void ImGuiGui::closeEditor() {
  if (!editorOpen_) return;
  editorOpen_ = false;
  if (auto h = plugin_.hosted())
    if (!h->dead()) h->editorClose();
  if (win_) {
    win_->destroyEditorLeftovers();
    win_->placeEditor(0, 0, 0, 0, false);
  }
}

void ImGuiGui::beforeUnload(HostedPlugin* p) {
  if (editorOpen_ && plugin_.hosted() == p) {
    editorOpen_ = false;
    if (p && !p->dead()) p->editorClose();
    if (win_) win_->destroyEditorLeftovers();
  }
  editorW_ = editorH_ = 0;
  if (win_) win_->placeEditor(0, 0, 0, 0, false);
}

void ImGuiGui::onEditorResize(int width, int height) {
  editorW_ = width;
  editorH_ = height;
  if (pageNow_ == Page::Editor) {
    placeEditor();
    requestSize();
  }
}

void ImGuiGui::onPluginChanged() {
  if (plugin_.hosted()) browserForced_ = false;
  refreshPrograms();
  genValues_.clear();
  genTexts_.clear();
  applyPage();
}

void ImGuiGui::onCrash() {
  editorOpen_ = false;  // never call into it again
  if (win_) win_->placeEditor(0, 0, 0, 0, false);
  applyPage();
}

void ImGuiGui::onParamsChanged() {
  genTexts_.clear();
  refreshPrograms();
  dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// Event loop

void ImGuiGui::tick() {
  if (!ctx_ || !win_) return;
  ImGui::SetCurrentContext(ctx_);
  bool input = win_->pollEvents();
  uint64_t now = nowMs();
  if (now - lastPeriodic_ >= 30) {  // the Win32 GUI's 30 ms timer
    lastPeriodic_ = now;
    periodic();
  }
  if (input || dirty_) framesLeft_ = 3;  // ImGui settles hover / active state over a few frames
  ImGuiIO& io = ImGui::GetIO();
  bool mouseInside = io.MousePos.x > -1e30f;
  uint64_t interval = (wasScanning_ || pageNow_ == Page::Generic || mouseInside) ? 50 : 1000;
  if (framesLeft_ <= 0 && now - lastFrame_ < interval) return;
  if (framesLeft_ > 0) --framesLeft_;
  dirty_ = false;
  renderFrame();
  lastFrame_ = now;
}

void ImGuiGui::periodic() {
  ++timerTicks_;
  auto h = plugin_.hosted();
  if (editorOpen_ && h && !h->dead()) h->editorIdle();
  if (h && h->dead() && pageNow_ != Page::Message && !browserForced_) onCrash();

  if (timerTicks_ % 6 == 0) {  // ~180 ms
    bool running = Shared::get().scanner.running();
    if (running || wasScanning_) {
      refreshScanStatus();
      if (timerTicks_ % 30 == 0 || !running) refreshList();
      if (!running) wasScanning_ = false;
    }
    if (pageNow_ == Page::Generic) dirty_ = true;
  }
  if (timerTicks_ % 70 == 0 && pageNow_ == Page::Browser) {  // ~2 s: pick up scans by other instances
    if (Shared::get().db.reloadIfChanged()) refreshList();
    if (Shared::get().scanner.running() != lastProgress_.running) refreshScanStatus();
  }
}

void ImGuiGui::renderFrame() {
  if (!glReady_ || !win_->beginGl((int)width_, (int)height_, palette::kBg)) return;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)width_, (float)height_);
  uint64_t now = nowMs();
  io.DeltaTime = std::clamp((now - lastFrame_) / 1000.f, 1.f / 240.f, 0.5f);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##yov", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar();
  ImGui::PushFont(font_, kFontSize);
  auto* dl = ImGui::GetWindowDrawList();
  float tb = (float)S(kToolbarH);
  dl->AddRectFilled(ImVec2(0, tb), ImVec2((float)width_, (float)height_), col(palette::kBg));
  switch (pageNow_) {
    case Page::Browser: drawBrowser(); break;
    case Page::Generic: drawGeneric(); break;
    case Page::Message: drawMessage(); break;
    case Page::Editor: break;  // the plugin's own window covers the body
  }
  drawToolbar();
  drawDialogs();
  drawFolderPicker();
  ImGui::PopFont();
  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  win_->endGl(true);
}

// ---------------------------------------------------------------------------------------------
// Widgets

bool ImGuiGui::button(const char* id, const char* label, float x, float y, float w, float h, bool primary,
                      bool enabled, const char* tip) {
  ImGui::SetCursorPos(ImVec2(x, y));
  bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h)) && enabled;
  if (tip) ImGui::SetItemTooltip("%s", tip);
  bool held = ImGui::IsItemActive() && enabled;
  bool hovered = ImGui::IsItemHovered() && enabled;
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  uint32_t fill = primary ? palette::kAccent : palette::kCtrl;
  if (held) fill = primary ? palette::kAccentPressed : palette::kCtrlHot;
  else if (hovered && !primary) fill = palette::kCtrlHot;
  if (!enabled) fill = palette::kCtrl;
  uint32_t border = primary && enabled ? palette::kAccent : palette::kBorder;
  auto* dl = ImGui::GetWindowDrawList();
  float r = (float)S(3);
  dl->AddRectFilled(a, b, col(fill), r);
  dl->AddRect(a, b, col(border), r);
  uint32_t tc = !enabled ? palette::kGrey : primary ? palette::kAccentText : palette::kText;
  ImFont* f = primary ? fontBold_ : font_;
  ImGui::PushFont(f, kFontSize);
  std::string t = ellipsize(label, w - S(8));
  ImVec2 ts = ImGui::CalcTextSize(t.c_str());
  dl->AddText(ImVec2(std::floor(a.x + (w - ts.x) / 2), std::floor(a.y + (h - ts.y) / 2)), col(tc), t.c_str());
  ImGui::PopFont();
  return pressed;
}

bool ImGuiGui::toggle(const char* id, const char* label, bool& value, float x, float y, float w, float h,
                      const char* tip) {
  ImGui::SetCursorPos(ImVec2(x, y));
  bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
  if (tip) ImGui::SetItemTooltip("%s", tip);
  if (pressed) value = !value;
  ImVec2 a = ImGui::GetItemRectMin();
  auto* dl = ImGui::GetWindowDrawList();
  float box = (float)S(14);
  ImVec2 b0(a.x + S(2), a.y + (h - box) / 2), b1(b0.x + box, b0.y + box);
  dl->AddRectFilled(b0, b1, col(value ? palette::kAccent : palette::kCtrl), (float)S(2));
  dl->AddRect(b0, b1, col(value ? palette::kAccent : palette::kBorder), (float)S(2));
  if (value) {
    ImVec2 pts[3] = {ImVec2(b0.x + box * 3 / 14, b0.y + box * 7 / 14), ImVec2(b0.x + box * 6 / 14, b0.y + box * 10 / 14),
                     ImVec2(b0.x + box * 11 / 14, b0.y + box * 4 / 14)};
    dl->AddPolyline(pts, 3, col(palette::kAccentText), 0, std::max(1.f, (float)S(2)));
  }
  float tx = b1.x + S(6);
  std::string t = ellipsize(label, a.x + w - tx);
  ImVec2 ts = ImGui::CalcTextSize(t.c_str());
  dl->AddText(ImVec2(tx, std::floor(a.y + (h - ts.y) / 2)), col(palette::kText), t.c_str());
  return pressed;
}

// ---------------------------------------------------------------------------------------------
// Toolbar

void ImGuiGui::drawToolbar() {
  auto* dl = ImGui::GetWindowDrawList();
  float w = (float)width_;
  float tb = (float)S(kToolbarH), pad = (float)S(8), bh = (float)S(26);
  float y = std::floor((tb - bh) / 2);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, tb), col(palette::kPanel));
  dl->AddRectFilled(ImVec2(0, tb - 1), ImVec2(w, tb), col(palette::kBorder));

  auto h = plugin_.hosted();
  Page p = pageNow_;
  bool loaded = h != nullptr;
  auto& sh = Shared::get();
  const char* browseLabel = p == Page::Browser && loaded ? "Back" : "Plugins";
  if (button("##browse", browseLabel, pad, y, (float)S(96), bh, false)) {
    if (currentPage() == Page::Browser && plugin_.hosted()) {
      browserForced_ = false;
    } else {
      browserForced_ = true;
      sh.db.reloadIfChanged();
      refreshList();
    }
    applyPage();
  }

  float right = w - pad;
  float titleRight = right;
  if (loaded && p != Page::Browser) {
    right -= S(70);
    if (button("##unload", "Unload", right, y, (float)S(70), bh, false)) {
      plugin_.unloadVst();
      browserForced_ = true;
      applyPage();
      return;
    }
    right -= S(6);
  }
  h = plugin_.hosted();
  bool canToggle = h && h->info().hasEditor && !h->dead() && p != Page::Browser;
  if (canToggle) {
    right -= S(92);
    if (button("##view", plugin_.genericView() ? "Plugin UI" : "Generic UI", right, y, (float)S(92), bh, false)) {
      plugin_.setGenericView(!plugin_.genericView());
      applyPage();
    }
    right -= S(6);
  }
  bool programs = h && !h->dead() && h->info().numPrograms > 1 && p != Page::Browser && !programNames_.empty();
  if (programs) {
    float comboW = std::clamp(w / 4, (float)S(120), (float)S(220));
    right -= comboW;
    ImGui::SetCursorPos(ImVec2(right, y + 1));
    ImGui::SetNextItemWidth(comboW);
    float fh = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2((float)S(6), std::max(0.f, (bh - 2 - fh) / 2)));
    int cur = std::clamp(currentProgram_, 0, (int)programNames_.size() - 1);
    std::string preview = std::to_string(cur + 1) + ": " + programNames_[cur];
    if (ImGui::BeginCombo("##program", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
      for (int i = 0; i < (int)programNames_.size(); ++i) {
        std::string item = std::to_string(i + 1) + ": " + programNames_[i] + "##p" + std::to_string(i);
        if (ImGui::Selectable(item.c_str(), i == cur)) {
          plugin_.setProgram(i);
          currentProgram_ = i;
          genTexts_.clear();
        }
        if (i == cur) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopStyleVar();
    right -= S(6);
  }
  titleRight = right - S(8) + S(6);

  // Title area between the Plugins button and the right-hand controls.
  float left = (float)(S(8) + S(96) + S(12));
  std::string title, sub;
  uint32_t subColor = palette::kDim;
  if (p == Page::Browser) {
    title = "YeOldeVST";
    sub = h ? "Pick a plugin to replace " + plugin_.loadedRecord().displayName()
            : (plugin_.isInstrument() ? "Pick a VST instrument" : "Pick a VST effect");
  } else if (h) {
    title = plugin_.loadedRecord().displayName();
    std::string vendor = h->info().vendor.empty() ? plugin_.loadedRecord().vendor : h->info().vendor;
    sub = vendor;
    if (h->sandboxed()) {
      sub += vendor.empty() ? "" : "  \xE2\x80\xA2  ";
      sub += plugin_.loadedRecord().arch == "x86" ? "32-bit, bridged" : "sandboxed";
    }
    if (h->dead()) {
      sub = "crashed";
      subColor = palette::kBad;
    }
  } else {
    title = "YeOldeVST";
    sub = "missing plugin";
    subColor = palette::kWarn;
  }
  if (titleRight > left) {
    ImGui::PushFont(fontBold_, kFontSizeTitle);
    std::string t = ellipsize(title, titleRight - left);
    float th = ImGui::GetFontSize();
    dl->AddText(ImVec2(left, std::floor(tb / 2 + S(3) - th)), col(palette::kText), t.c_str());
    ImGui::PopFont();
    ImGui::PushFont(font_, kFontSizeSmall);
    std::string s = ellipsize(sub, titleRight - left);
    dl->AddText(ImVec2(left, std::floor(tb / 2 + S(2))), col(subColor), s.c_str());
    ImGui::PopFont();
  }
}

// ---------------------------------------------------------------------------------------------
// Browser

void ImGuiGui::drawBrowser() {
  auto& sh = Shared::get();
  float w = (float)width_, h = (float)height_ - S(kToolbarH);
  float top = (float)S(kToolbarH);
  float pad = (float)S(10), bh = (float)S(26), btnW = (float)S(110);
  float y = top + pad;
  ImGui::PushFont(font_, kFontSizeSmall);
  ImGui::GetWindowDrawList()->AddText(ImVec2(pad, y), col(palette::kDim), "VST2 PLUGIN FOLDERS");
  ImGui::PopFont();
  y += S(20);
  float foldersH = (float)S(70);
  // folder list
  ImGui::SetCursorPos(ImVec2(pad, y));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, colv(palette::kCtrl));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2((float)S(3), (float)S(1)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  if (ImGui::BeginChild("##folders", ImVec2(w - pad * 3 - btnW, foldersH), ImGuiChildFlags_AlwaysUseWindowPadding)) {
    auto& folders = sh.settings.folders;
    for (int i = 0; i < (int)folders.size(); ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(folders[i].c_str(), folderSel_ == i)) folderSel_ = i;
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (button("##addfolder", "Add folder...", w - pad - btnW, y, btnW, bh, false)) addFolder();
  if (button("##removefolder", "Remove", w - pad - btnW, y + bh + S(6), btnW, bh, false)) removeFolder();
  y += foldersH + S(8);

  bool running = sh.scanner.running();
  float x = pad;
  if (button("##scan", "Scan", x, y, (float)S(96), bh, true, !running,
             "Scan new and changed plugins in the folders above. Each plugin is tested in its own throwaway "
             "process, so a crashing plugin cannot take your DAW down."))
    startScan(false, false);
  x += S(96) + S(6);
  if (button("##rescan", "Rescan all", x, y, (float)S(96), bh, false, !running,
             "Forget all results and test every plugin again."))
    startScan(true, false);
  x += S(96) + S(6);
  if (button("##retry", "Rescan failed", x, y, (float)S(96), bh, false, !running,
             "Test again only the plugins that crashed, hung or failed to load (including ones whose editor "
             "crashed). Everything else keeps its result."))
    startScan(false, true);
  x += S(96) + S(6);
  if (button("##stop", "Stop", x, y, (float)S(64), bh, false, running)) sh.scanner.cancel();
  x += S(64) + S(16);
  float t1 = (float)S(116), t2 = (float)S(230);
  if (x + t1 + t2 + pad <= w) {
    bool changed = toggle("##tgleditors", "Test editors", testEditors_, x, y, t1, bh,
                          "Also open and close each plugin's editor window during the scan. Slower, but catches "
                          "plugins whose UI crashes. Editors that already passed are not tested again; with this "
                          "off, the Editor column shows 'not tested' for plugins never tested.");
    changed |= toggle("##tglhidden", "Hide plugin popups during scan", hiddenDesktop_, x + t1 + S(4), y, t2, bh,
#if defined(_WIN32)
                      "Run the scan on a hidden desktop so nag screens and licence dialogs don't pop up. "
#else
                      "Run the scan on a hidden display (Xvfb, if installed) so nag screens and licence dialogs "
                      "don't pop up. "
#endif
                      "Plugins that wait for such a dialog are reported as 'Hangs'.");
    if (changed) {
      sh.settings.testEditors = testEditors_;
      sh.settings.hiddenDesktop = hiddenDesktop_;
      sh.settings.save();
    }
  }
  y += bh + S(10);

  // search + filter
  float filterW = (float)S(150);
  float fh = ImGui::GetFontSize();
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2((float)S(6), std::max(0.f, (bh - fh) / 2)));
  ImGui::SetCursorPos(ImVec2(pad, y));
  ImGui::SetNextItemWidth(w - pad * 3 - filterW);
  if (ImGui::InputTextWithHint("##search", "Search name, vendor, file...", search_, sizeof search_,
                               ImGuiInputTextFlags_EscapeClearsAll))
    refreshList();
  ImGui::SetCursorPos(ImVec2(w - pad - filterW, y));
  ImGui::SetNextItemWidth(filterW);
  if (ImGui::BeginCombo("##filter", kFilterNames[filter_])) {
    for (int i = 0; i < F_COUNT; ++i)
      if (ImGui::Selectable(kFilterNames[i], i == filter_)) {
        filter_ = i;
        refreshList();
      }
    ImGui::EndCombo();
  }
  ImGui::PopStyleVar();
  y += bh + S(8);

  float bottomH = (float)S(40);
  float listH = std::max((float)S(40), top + h - y - bottomH);
  drawPluginList(pad, y, w - pad * 2, listH);

  // bottom: status + progress, Load sandboxed, Load
  float by = top + h - bottomH + (bottomH - bh) / 2;
  float lw = (float)S(90), sw = (float)S(130);
  bool haveSel = model_.rowRecord(selRow_) != nullptr;
  if (button("##loadsb", "Load sandboxed", w - pad - lw - S(6) - sw, by, sw, bh, false, true,
             "Run the plugin in a separate helper process. If it crashes, only that process dies: your DAW keeps "
             "running and you can reload it. 32-bit plugins always run this way."))
    loadSelected(true);
  if (button("##load", "Load", w - pad - lw, by, lw, bh, true)) loadSelected(false);
  (void)haveSel;
  float statusRight = std::max(pad, w - pad - lw - S(6) - sw - S(12));
  auto& p = lastProgress_;
  std::string status = scanStatusLine(p, sh.db);
  auto* dl = ImGui::GetWindowDrawList();
  std::string st = ellipsize(status, statusRight - pad);
  float th = ImGui::GetFontSize();
  dl->AddText(ImVec2(pad, std::floor(by - S(2) + (bh - S(4) - th) / 2)), col(p.running ? palette::kText : palette::kDim),
              st.c_str());
  if (p.running && p.total > 0) {
    ImVec2 a(pad, by + bh - S(4)), b(statusRight, by + bh);
    dl->AddRectFilled(a, b, col(palette::kCtrl));
    dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * (float)p.done / (float)p.total, b.y), col(palette::kAccent));
  }
}

void ImGuiGui::drawPluginList(float x, float y, float w, float h) {
  ImGui::SetCursorPos(ImVec2(x, y));
  ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable |
                          ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_BordersOuter;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, colv(palette::kPanel));
  ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, colv(palette::kPanel));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2((float)S(6), 0));
  if (!ImGui::BeginTable("##plugins", kColumnCount, flags, ImVec2(w, h))) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return;
  }
  for (int i = 0; i < kColumnCount; ++i)
    ImGui::TableSetupColumn(kColumnNames[i],
                            ImGuiTableColumnFlags_WidthFixed | (i == 0 ? ImGuiTableColumnFlags_DefaultSort : 0) |
                                ImGuiTableColumnFlags_NoHide,
                            (float)S((float)kColumnWidths[i]));
  ImGui::TableSetupScrollFreeze(0, 1);
  // Header row, as tall as the Win32 list's.
  ImGui::PushFont(fontBold_, kFontSize);
  ImGui::TableNextRow(ImGuiTableRowFlags_Headers, (float)S(22));
  for (int i = 0; i < kColumnCount; ++i) {
    if (!ImGui::TableSetColumnIndex(i)) continue;
    float pad = (S(22) - ImGui::GetFontSize()) / 2;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad);
    ImGui::TableHeader(kColumnNames[i]);
  }
  ImGui::PopFont();
  if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
    if (specs->SpecsDirty && specs->SpecsCount > 0) {
      model_.sortColumn = specs->Specs[0].ColumnIndex;
      model_.sortAscending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
      refreshList();
      specs->SpecsDirty = false;
    }
  }

  const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
  const int rows = model_.rowCount();
  float rowH = std::floor(ImGui::GetFontSize() + S(6));
  bool openContext = false;
  ImGuiListClipper clipper;
  clipper.Begin(rows, rowH);
  if (scrollToRow_ >= 0 && scrollToRow_ < rows) clipper.IncludeItemByIndex(scrollToRow_);
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      ImGui::TableNextRow(0, rowH);
      bool selected = row == selRow_;
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                             col(selected ? palette::kSelect : (row % 2 ? palette::kRowAlt : palette::kPanel)));
      const bool group = model_.rowIsGroup(row);
      ImGui::TableSetColumnIndex(0);
      const ImVec2 cell0 = ImGui::GetCursorScreenPos();
      float cellX = cell0.x;
      ImGui::PushID(row);
      ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
      ImGui::Selectable("##row", selected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                            ImGuiSelectableFlags_AllowDoubleClick,
                        ImVec2(0, rowH));
      ImGui::PopStyleColor(2);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        selRow_ = row;
        // Clicking the arrow of a group row opens / closes it.
        if (group && ImGui::GetMousePos().x - cellX < S(22)) {
          model_.toggle(row);
          refreshList();
        }
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        selRow_ = row;
        if (group) {
          model_.toggle(row);
          refreshList();
        } else {
          loadSelected(false);
        }
      }
      // Right-click works even while the search field has the keyboard (like the Win32 list).
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        ImGui::ClearActiveID();
        selRow_ = row;
        ctxRow_ = row;
        openContext = true;  // opened outside the row's ID scope, where BeginPopup looks for it
      }
      if (row < model_.rowCount() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", model_.tooltip(row).c_str());
      if (row == scrollToRow_) {
        ImGui::SetScrollHereY();
        scrollToRow_ = -1;
      }
      ImGui::PopID();
      if (row >= model_.rowCount()) break;  // the list changed under us (toggle)

      ImGui::PushFont(group ? fontBold_ : font_, kFontSize);
      for (int c = 0; c < kColumnCount; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        float cw = ImGui::GetContentRegionAvail().x;
        // Column 0's cursor already moved below the row's Selectable: use where the cell starts.
        ImVec2 pos = c == 0 ? cell0 : ImGui::GetCursorScreenPos();
        std::string t = ellipsize(model_.cellText(row, c), cw);
        float th = ImGui::GetFontSize();
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + std::floor((rowH - th) / 2)),
                                            col(model_.cellColor(row, c)), t.c_str());
      }
      ImGui::PopFont();
    }
  }
  scrollToRow_ = -1;
  if (openContext) ImGui::OpenPopup("##ctx");

  // Keyboard: Up / Down select, Enter loads, Right / Left open and close a group.
  if (focused && rows > 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    int pageRows = std::max(1, (int)(h / rowH) - 2);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) selectRow(std::min(rows - 1, selRow_ + 1), true);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) selectRow(std::max(0, selRow_ - 1), true);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) selectRow(std::min(rows - 1, selRow_ + pageRows), true);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) selectRow(std::max(0, selRow_ - pageRows), true);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) selectRow(0, true);
    if (ImGui::IsKeyPressed(ImGuiKey_End)) selectRow(rows - 1, true);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) loadSelected(false);
    bool open = model_.rowExpanded(selRow_);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && model_.rowIsGroup(selRow_) && !open) {
      model_.toggle(selRow_);
      refreshList();
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && model_.rowIsGroup(selRow_) && open) {
      model_.toggle(selRow_);
      refreshList();
    }
  }

  // Context menu
  if (ImGui::BeginPopup("##ctx")) {
    const PluginRecord* rp = model_.rowRecord(ctxRow_);
    if (!rp) {
      ImGui::CloseCurrentPopup();
    } else {
      const PluginRecord r = *rp;
      const bool group = model_.rowIsGroup(ctxRow_);
      const bool busy = Shared::get().scanner.running();
      std::string loadText = group ? "Load best copy (" + fileNameOf(r.path) + ")" : std::string("Load");
      if (ImGui::MenuItem(loadText.c_str())) loadRecord(r, false);
      if (ImGui::MenuItem("Load sandboxed (separate process)")) loadRecord(r, true);
      ImGui::Separator();
      if (group) {
        if (ImGui::MenuItem(model_.rowExpanded(ctxRow_) ? "Collapse" : "Show all copies")) {
          model_.toggle(ctxRow_);
          refreshList();
        }
      }
      if (ImGui::MenuItem(group ? "Test all copies again" : "Test again", nullptr, false, !busy)) {
        std::vector<std::string> files;
        if (group)
          for (auto& c : model_.rowGroup(ctxRow_)->recs) files.push_back(c.path);
        else
          files.push_back(r.path);
        startScan(false, false, files);
      }
      std::string reveal = std::string("Show in ") + fileManagerName();
      if (ImGui::MenuItem(reveal.c_str())) win_->revealInFileManager(r.path);
      if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(r.path.c_str());
    }
    ImGui::EndPopup();
  }
  ImGui::EndTable();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
}

void ImGuiGui::selectRow(int row, bool scroll) {
  selRow_ = row;
  if (scroll) scrollToRow_ = row;
  dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// Generic editor

void ImGuiGui::drawGeneric() {
  float top = (float)S(kToolbarH);
  float w = (float)width_, h = (float)height_ - top;
  auto hp = plugin_.hosted();
  auto& names = plugin_.paramNames();
  auto* dl = ImGui::GetWindowDrawList();
  if (!hp || hp->dead()) return;
  if (names.empty()) {
    const char* t = "This plugin has no parameters to show.";
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2((w - ts.x) / 2, top + (h - ts.y) / 2), col(palette::kDim), t);
    return;
  }
  if (genValues_.size() != names.size()) genValues_.assign(names.size(), -1.f);
  if (genTexts_.size() != names.size()) genTexts_.assign(names.size(), std::string());
  ImGui::SetCursorPos(ImVec2(0, top));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, colv(palette::kBg));
  if (ImGui::BeginChild("##generic", ImVec2(w, h), 0, ImGuiWindowFlags_NoMove)) {
    auto* cdl = ImGui::GetWindowDrawList();
    float rowH = (float)S(kGenericRowH);
    float cw = ImGui::GetContentRegionAvail().x;
    float nameW = cw * 38 / 100, valueW = cw * 20 / 100;
    float barL = nameW + S(8), barR = cw - valueW - S(8);
    ImGuiListClipper clipper;
    clipper.Begin((int)names.size(), rowH);
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        ImGui::SetCursorPos(ImVec2(0, i * rowH));
        ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImGui::InvisibleButton("##p", ImVec2(cw, rowH));
        if (ImGui::IsItemActivated()) {
          genDragRow_ = i;
          plugin_.beginParameterEdit((uint32_t)i);
        }
        if ((ImGui::IsItemActivated() || (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0)) &&
            barR > barL) {
          double v = std::clamp((double)(ImGui::GetMousePos().x - o.x - barL) / (barR - barL), 0.0, 1.0);
          plugin_.editParameter((uint32_t)i, v);
          genTexts_[i].clear();
        }
        if (ImGui::IsItemDeactivated() && genDragRow_ == i) {
          plugin_.endParameterEdit((uint32_t)i);
          genDragRow_ = -1;
        }
        ImGui::PopID();
        cdl->AddRectFilled(o, ImVec2(o.x + cw, o.y + rowH), col(i % 2 ? palette::kRowAlt : palette::kBg));
        float v = std::clamp(hp->getParameter(i), 0.f, 1.f);
        if (v != genValues_[i] || genTexts_[i].empty()) {
          genValues_[i] = v;
          genTexts_[i] = hp->paramDisplay(i);
          if (genTexts_[i].empty()) {
            char b[32];
            snprintf(b, sizeof b, "%.1f%%", v * 100.0);
            genTexts_[i] = b;
          }
        }
        float th = ImGui::GetFontSize();
        float ty = std::floor(o.y + (rowH - th) / 2);
        std::string nm = ellipsize(names[i], nameW - S(10));
        cdl->AddText(ImVec2(o.x + S(10), ty), col(i == genDragRow_ ? palette::kAccent : palette::kText), nm.c_str());
        float cy = o.y + rowH / 2;
        ImVec2 t0(o.x + barL, cy - S(3)), t1(o.x + barR, cy + S(3));
        cdl->AddRectFilled(t0, t1, col(palette::kCtrl), (float)S(3));
        float fx = o.x + barL + (barR - barL) * v;
        if (fx > t0.x + 1) cdl->AddRectFilled(t0, ImVec2(fx, t1.y), col(palette::kAccent), (float)S(3));
        cdl->AddRectFilled(ImVec2(fx - S(5), cy - S(7)), ImVec2(fx + S(5), cy + S(7)), col(palette::kText), (float)S(2));
        std::string vt = ellipsize(genTexts_[i], valueW - S(10));
        float vw = ImGui::CalcTextSize(vt.c_str()).x;
        cdl->AddText(ImVec2(o.x + cw - S(10) - vw, ty), col(palette::kDim), vt.c_str());
      }
    }
    ImGui::SetCursorPos(ImVec2(0, names.size() * rowH));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------
// Message page

void ImGuiGui::drawMessage() {
  float tb = (float)S(kToolbarH);
  float w = (float)width_, h = (float)height_;
  auto hp = plugin_.hosted();
  std::string msg;
  if (hp && hp->dead()) {
    msg = plugin_.loadedRecord().displayName() +
          " crashed and has been switched off. Your DAW is fine; the track is muted until you reload it.\n\n" +
          hp->deathReason();
  } else {
    msg = plugin_.missingDescription();
  }
  float left = (float)S(30), right = w - S(30);
  float bottom = tb + (h - tb) / 2 + S(20);
  auto lines = wrapLines(msg, right - left);
  float lh = ImGui::GetTextLineHeight();
  float y = bottom - lh * lines.size();
  y = std::max(y, tb + S(30));
  auto* dl = ImGui::GetWindowDrawList();
  for (auto& l : lines) {
    float lw = ImGui::CalcTextSize(l.c_str()).x;
    dl->AddText(ImVec2(std::floor(left + (right - left - lw) / 2), y), col(palette::kText), l.c_str());
    y += lh;
  }
  if (hp) {
    if (button("##reload", "Reload plugin", (w - S(140)) / 2, tb + (h - tb) / 2 + S(30), (float)S(140), (float)S(30),
               true, true, "Start the plugin again and restore its last saved settings.")) {
      std::string err;
      if (!plugin_.reload(err)) message("Reload failed:\n\n" + err, 2);
      applyPage();
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Dialogs

void ImGuiGui::message(const std::string& text, int kind, int buttons, std::function<void(int)> done) {
  if (win_) {
    int r = win_->messageBox(text, kind, buttons);
    if (r >= 0) {
      if (done) done(r);
      return;
    }
  }
  dialogs_.push_back({text, kind, buttons, std::move(done)});
  dirty_ = true;
}

void ImGuiGui::drawDialogs() {
  if (dialogs_.empty()) return;
  if (!ImGui::IsPopupOpen("##dialog")) ImGui::OpenPopup("##dialog");
  ImGuiIO& io = ImGui::GetIO();
  float dw = std::min((float)S(460), io.DisplaySize.x - S(20));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(dw, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2((float)S(16), (float)S(14)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, colv(palette::kPanel));
  if (ImGui::BeginPopupModal("##dialog", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    Dialog& d = dialogs_.front();
    uint32_t accent = d.kind == 2 ? palette::kBad : d.kind == 1 ? palette::kWarn : palette::kAccent;
    ImVec2 p = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + S(4), p.y + ImGui::GetWindowHeight()), col(accent));
    ImGui::PushTextWrapPos(dw - S(16));
    ImGui::TextUnformatted(d.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(1, (float)S(10)));
    std::vector<std::pair<const char*, int>> btns;
    if (d.buttons == 2) btns = {{"Yes", 0}, {"No", 1}, {"Cancel", 2}};
    else if (d.buttons == 1) btns = {{"OK", 0}, {"Cancel", 2}};
    else btns = {{"OK", 0}};
    float bw = (float)S(84), bh = (float)S(26), gap = (float)S(6);
    float x = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - (bw * btns.size() + gap * (btns.size() - 1));
    float y = ImGui::GetCursorPosY();
    int result = -1;
    for (size_t i = 0; i < btns.size(); ++i) {
      std::string id = std::string("##dlg") + btns[i].first;
      if (button(id.c_str(), btns[i].first, x + i * (bw + gap), y, bw, bh, i == 0)) result = btns[i].second;
    }
    ImGui::SetCursorPosY(y + bh);
    ImGui::Dummy(ImVec2(1, 1));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) result = d.buttons == 0 ? 0 : 2;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) result = 0;
    if (result >= 0) {
      auto done = std::move(d.done);
      dialogs_.erase(dialogs_.begin());
      ImGui::CloseCurrentPopup();
      if (done) done(result);
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void ImGuiGui::drawFolderPicker() {
  if (!folderPicker_) return;
  auto list = [&] {
    pickEntries_.clear();
    std::vector<DirEntry> es;
    if (listDir(pickDir_, es))
      for (auto& e : es)
        if (e.isDir && !e.name.empty() && e.name[0] != '.') pickEntries_.push_back(e.name);
    std::sort(pickEntries_.begin(), pickEntries_.end(),
              [](const std::string& a, const std::string& b) { return foldCase(a) < foldCase(b); });
    pickSel_ = -1;
    snprintf(pickPath_, sizeof pickPath_, "%s", pickDir_.c_str());
  };
  if (!ImGui::IsPopupOpen("##folderpicker")) {
    ImGui::OpenPopup("##folderpicker");
    list();
  }
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min((float)S(560), io.DisplaySize.x - S(20)),
                                  std::min((float)S(420), io.DisplaySize.y - S(20))));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2((float)S(14), (float)S(12)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, colv(palette::kPanel));
  if (ImGui::BeginPopupModal("##folderpicker", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    std::string title = std::string("Choose a folder with ") + pluginFileKind();
    ImGui::PushFont(fontBold_, kFontSize);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    float bw = (float)S(84), bh = (float)S(26);
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(avail - bw - S(6));
    if (ImGui::InputText("##pickpath", pickPath_, sizeof pickPath_, ImGuiInputTextFlags_EnterReturnsTrue) &&
        dirExists(pickPath_)) {
      pickDir_ = pickPath_;
      list();
    }
    ImGui::SameLine(0, (float)S(6));
    float py = ImGui::GetCursorPosY();
    if (button("##pickup", "Up", ImGui::GetCursorPosX(), py, bw, ImGui::GetFrameHeight(), false)) {
      std::string up = dirOf(pickDir_);
      if (!up.empty() && up != pickDir_) {
        pickDir_ = up;
        list();
      }
    }
    float listH = ImGui::GetContentRegionAvail().y - bh - S(12);
    ImGui::SetCursorPosY(py + ImGui::GetFrameHeight() + S(6));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colv(palette::kCtrl));
    std::string enter;
    if (ImGui::BeginChild("##pickentries", ImVec2(avail, listH))) {
      for (int i = 0; i < (int)pickEntries_.size(); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(pickEntries_[i].c_str(), pickSel_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
          pickSel_ = i;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) enter = pickEntries_[i];
        }
        ImGui::PopID();
      }
      if (pickEntries_.empty()) ImGui::TextDisabled("(no subfolders)");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (!enter.empty()) {
      pickDir_ = joinPath(pickDir_, enter);
      list();
    }
    float by = ImGui::GetCursorPosY() + S(6);
    float right = ImGui::GetCursorPosX() + avail;
    bool close = false;
    if (button("##pickcancel", "Cancel", right - bw, by, bw, bh, false) || ImGui::IsKeyPressed(ImGuiKey_Escape))
      close = true;
    if (button("##pickok", "Add folder", right - bw * 2 - S(6), by, bw, bh, true)) {
      std::string chosen = pickSel_ >= 0 && pickSel_ < (int)pickEntries_.size() ? joinPath(pickDir_, pickEntries_[pickSel_])
                                                                                : pickDir_;
      close = true;
      addFolderPath(chosen);
    }
    if (close) {
      folderPicker_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------------------------
// Browser actions

void ImGuiGui::refreshFolders() {
  auto& st = Shared::get().settings;
  if (st.folders.empty()) {
    st.folders = defaultVstFolders();
    if (!st.folders.empty()) st.save();
  }
  if (folderSel_ >= (int)st.folders.size()) folderSel_ = -1;
}

void ImGuiGui::addFolder() {
  std::string path;
  std::string title = std::string("Choose a folder with ") + pluginFileKind();
  if (win_ && win_->pickFolder(title.c_str(), path)) {
    addFolderPath(path);
    return;
  }
  // Our own picker: start next to the last folder, or at home.
  auto& st = Shared::get().settings;
  pickDir_.clear();
  if (!st.folders.empty()) pickDir_ = dirOf(st.folders.back());
  if (pickDir_.empty() || !dirExists(pickDir_)) {
    getEnv("HOME", pickDir_);
    if (pickDir_.empty()) getEnv("USERPROFILE", pickDir_);
  }
  if (pickDir_.empty()) pickDir_ = "/";
  folderPicker_ = true;
  dirty_ = true;
}

void ImGuiGui::addFolderPath(const std::string& p) {
  if (p.empty()) return;
  auto& st = Shared::get().settings;
  for (auto& f : st.folders)
    if (pathKey(f) == pathKey(p)) return;
  st.folders.push_back(p);
  st.save();
  refreshFolders();
  startScan(false, false);
}

void ImGuiGui::removeFolder() {
  auto& st = Shared::get().settings;
  int sel = folderSel_;
  if (sel < 0 || sel >= (int)st.folders.size()) return;
  st.folders.erase(st.folders.begin() + sel);
  st.save();
  folderSel_ = -1;
  refreshFolders();
  Shared::get().db.prune(st.folders);
  Shared::get().db.save();
  refreshList();
}

void ImGuiGui::startScan(bool rescanAll, bool retryFailed, const std::vector<std::string>& onlyFiles) {
  auto& sh = Shared::get();
  if (sh.scanner.running()) return;
  if (sh.settings.folders.empty() && onlyFiles.empty()) {
    message(std::string("Add a folder with ") + pluginFileKind() + " first.", 0);
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

void ImGuiGui::refreshList() {
  std::string selId = model_.rowId(selRow_);
  selRow_ = model_.rebuild(Shared::get().db.all(), filter_, search_, selId);
  dirty_ = true;
}

void ImGuiGui::refreshPrograms() {
  programNames_.clear();
  auto h = plugin_.hosted();
  if (!h || h->dead() || h->info().numPrograms <= 1) return;
  programNames_ = h->programNames();
  currentProgram_ = h->currentProgram();
  dirty_ = true;
}

void ImGuiGui::refreshScanStatus() {
  lastProgress_ = Shared::get().scanner.progress();
  dirty_ = true;
}

void ImGuiGui::loadSelected(bool sandboxed) {
  // A group row loads its best copy (working, preferably 64-bit); expand it to pick another.
  if (const PluginRecord* r = model_.rowRecord(selRow_)) loadRecord(*r, sandboxed);
}

void ImGuiGui::loadRecord(const PluginRecord& r, bool sandboxed) {
  if (r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported) {
    message(r.displayName() + " can't be loaded: " + r.detail, 0);
    return;
  }
  LoadQuestion q = loadQuestion(r, sandboxed);
  if (!q.text.empty()) {
    message(q.text, 1, q.offerSandbox ? 2 : 1, [this, r, sandboxed, q](int res) {
      if (res == 2) return;
      doLoad(r, sandboxed || (q.offerSandbox && res == 0));
    });
    return;
  }
  doLoad(r, sandboxed);
}

void ImGuiGui::doLoad(const PluginRecord& r, bool sandboxed) {
  std::string err;
  if (!plugin_.loadVst(r, sandboxed, err)) {
    message("Could not load " + r.displayName() + ":\n\n" + err, 2);
    return;
  }
  browserForced_ = false;
  applyPage();
}

}  // namespace

std::unique_ptr<Gui> makeImGuiGui(Plugin& plugin) { return std::make_unique<ImGuiGui>(plugin); }

void imguiGuiShutdown() { guiWindowShutdown(); }

}  // namespace yov
