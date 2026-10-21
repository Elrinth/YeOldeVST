// The native side of the Dear ImGui GUI: a child window of the host's window with an OpenGL
// context for ImGui, a native child window the hosted plugin's editor opens in, input fed into
// the current ImGui context, and a periodic tick on the host's main thread.
#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace yov {

class Plugin;

// Before the plugin binary is unloaded: undo process-wide changes (X11: the error handler).
void guiWindowShutdown();

class GuiWindow {
 public:
  virtual ~GuiWindow() = default;

  // The implementation for this platform (X11 on Linux, Win32 + WGL on Windows).
  static std::unique_ptr<GuiWindow> create(Plugin& plugin);

  // Creates the windows inside the host's window and the GL context.
  virtual bool attach(const clap_window_t* parent, int width, int height) = 0;
  virtual void resize(int width, int height) = 0;
  virtual void setVisible(bool visible) = 0;
  // 1.0 at 96 dpi.
  virtual float scale() const = 0;

  // Calls tick() every few milliseconds on the main thread until the window goes away.
  virtual void startTicking(std::function<void()> tick) = 0;
  // CLAP timer / fd callbacks from the host (X11).
  virtual void onHostTimer(clap_id) {}
  virtual void onHostFd(int, clap_posix_fd_flags_t) {}

  // Feeds pending window-system input into the current ImGui context. True if there was any
  // (or the window needs a redraw).
  virtual bool pollEvents() = 0;
  // Makes the GL context current (remembering whatever was current before) and clears a
  // width x height viewport to clearRgb (0xRRGGBB).
  virtual bool beginGl(int width, int height, uint32_t clearRgb) = 0;
  // Shows what was drawn (swap) and puts the previous GL context back.
  virtual void endGl(bool swap) = 0;

  // The native window the plugin's editor opens in (HWND / X11 window id).
  virtual void* editorParent() = 0;
  virtual void placeEditor(int x, int y, int width, int height, bool visible) = 0;
  // Size of the first window the plugin created in the editor parent.
  virtual bool editorChildSize(int& width, int& height) = 0;
  // Removes windows a plugin left in the editor parent after closing its editor.
  virtual void destroyEditorLeftovers() = 0;

  // The system's folder picker, if there is one. False: none (the GUI shows its own).
  virtual bool pickFolder(const char* title, std::string& path) { return false; }
  // Shows a file in the system file manager.
  virtual void revealInFileManager(const std::string& path) = 0;
  // Native message box, if there is one that works here; -1 if not (the GUI draws its own).
  // kind: 0 info, 1 warning, 2 error. buttons: 0 OK, 1 OK/Cancel, 2 Yes/No/Cancel. Returns
  // 0 OK/Yes, 1 No, 2 Cancel.
  virtual int messageBox(const std::string& text, int kind, int buttons) { return -1; }
};

}  // namespace yov
