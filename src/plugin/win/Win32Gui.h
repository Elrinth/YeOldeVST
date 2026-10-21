// The Windows plugin window: plain Win32 + common controls, dark themed. A plugin browser
// (folders, scan, results) and the hosted plugin's own editor (or a generic slider editor when
// it has none / it is unusable).
#pragma once

#include "common/PluginDb.h"
#include "plugin/BrowserModel.h"
#include "plugin/Gui.h"
#include "scan/Scanner.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace yov {

class Plugin;
class HostedPlugin;

class Win32Gui final : public Gui {
 public:
  explicit Win32Gui(Plugin& plugin);
  ~Win32Gui() override;

  // CLAP gui
  bool setParent(const clap_window_t* window) override;
  void getSize(uint32_t& w, uint32_t& h) const override;
  void adjustSize(uint32_t& w, uint32_t& h) const override;
  bool setSize(uint32_t w, uint32_t h) override;
  void show() override;
  void hide() override;

  // Notifications from the plugin (main thread)
  void onPluginChanged() override;
  void onCrash() override;
  void onEditorResize(int width, int height) override;
  void onParamsChanged() override;
  void onParamValue(uint32_t index) override;
  void beforeUnload(HostedPlugin* p) override;

  static void unregisterClasses();

 private:
  enum class Page { Browser, Editor, Generic, Message };

  static LRESULT CALLBACK rootProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK pageProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK genericProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK listSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  LRESULT onRoot(HWND, UINT, WPARAM, LPARAM);
  LRESULT onPage(HWND, UINT, WPARAM, LPARAM);
  LRESULT onGeneric(HWND, UINT, WPARAM, LPARAM);

  void createControls();
  void createFonts();
  int S(int v) const { return MulDiv(v, dpi_, 96); }
  void layout();
  void layoutBrowser(int w, int h);
  Page currentPage() const;
  void applyPage();
  void preferredSize(uint32_t& w, uint32_t& h) const;
  void requestSize();

  // Commands
  void onCommand(int id, int code, HWND ctl);
  void addFolder();
  void removeFolder();
  void startScan(bool rescanAll, bool retryFailed, const std::vector<std::string>& onlyFiles = {});
  void loadSelected(bool sandboxed);
  void loadRecord(const PluginRecord& r, bool sandboxed);
  void showContextMenu(int x, int y);
  void refreshList();
  void refreshFolders();
  void refreshPrograms();
  void refreshScanStatus();
  void onTimer();

  // Editor
  void openEditor();
  void closeEditor();

  // Painting
  void paintRoot(HDC dc, const RECT& rc);
  void paintPage(HDC dc, const RECT& rc);
  void drawButton(const DRAWITEMSTRUCT* dis);
  void paintGeneric(HDC dc, const RECT& rc);
  LRESULT listCustomDraw(NMLVCUSTOMDRAW* cd);
  LRESULT headerCustomDraw(NMCUSTOMDRAW* cd);

  // Generic editor
  int genericRowAt(int y) const;
  void genericSetFromX(int row, int x);
  void genericScrollTo(int pos);
  void genericUpdateScroll();

  Plugin& plugin_;
  HWND parent_ = nullptr;
  HWND root_ = nullptr;
  HWND page_ = nullptr;        // browser page container
  HWND editorHost_ = nullptr;  // parent of the VST editor
  HWND generic_ = nullptr;
  int dpi_ = 96;
  uint32_t width_ = 0, height_ = 0;

  // toolbar
  HWND btnBrowse_ = nullptr, cmbProgram_ = nullptr, btnView_ = nullptr, btnUnload_ = nullptr, btnReload_ = nullptr;
  // browser page
  HWND lstFolders_ = nullptr, btnAddFolder_ = nullptr, btnRemoveFolder_ = nullptr;
  HWND btnScan_ = nullptr, btnRescan_ = nullptr, btnRetry_ = nullptr, btnStop_ = nullptr;
  HWND tglEditors_ = nullptr, tglHidden_ = nullptr;
  HWND edtSearch_ = nullptr, cmbFilter_ = nullptr, lv_ = nullptr;
  HWND btnLoad_ = nullptr, btnLoadSandboxed_ = nullptr;
  RECT statusRect_{}, progressRect_{}, foldersLabelRect_{};

  HFONT font_ = nullptr, fontBold_ = nullptr, fontTitle_ = nullptr, fontSmall_ = nullptr;
  HBRUSH brBg_ = nullptr, brPanel_ = nullptr, brCtrl_ = nullptr;

  bool browserForced_ = true;  // user asked for the browser
  Page page_now_ = Page::Browser;
  bool editorOpen_ = false;
  int editorW_ = 0, editorH_ = 0;
  std::map<HWND, bool> toggles_;

  // The list is a two-level tree: plugins with the same name and vendor (Sylenth1 x25, 32- and
  // 64-bit copies...) form one group row that expands to one row per file.
  BrowserModel model_;
  void toggleGroup(int row);
  bool wasScanning_ = false;
  int timerTicks_ = 0;
  ScanProgress lastProgress_;

  // generic editor state
  int genScroll_ = 0;
  int genDragRow_ = -1;
  std::vector<float> genValues_;
  std::vector<std::string> genTexts_;
};

}  // namespace yov
