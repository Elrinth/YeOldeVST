// The plugin window: a plugin browser (folders, scan, results) and the hosted plugin's own
// editor (or a generic slider editor when it has none / it is unusable).
//
// Two implementations: the native Win32 one (plugin/win, the default on Windows) and a
// Dear ImGui one (plugin/imgui) for Linux and macOS, which can also run on Windows
// (YEOLDEVST_GUI=imgui).
#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <memory>

namespace yov {

class Plugin;
class HostedPlugin;

class Gui {
 public:
  virtual ~Gui() = default;

  // The CLAP window API this platform's GUI embeds into.
  static const char* windowApi();
  static std::unique_ptr<Gui> create(Plugin& plugin);
  // When the plugin binary is about to be unloaded.
  static void shutdown();

  // CLAP gui
  virtual bool setParent(const clap_window_t* window) = 0;
  virtual void getSize(uint32_t& w, uint32_t& h) const = 0;
  virtual void adjustSize(uint32_t& w, uint32_t& h) const = 0;
  virtual bool setSize(uint32_t w, uint32_t h) = 0;
  virtual void show() = 0;
  virtual void hide() = 0;

  // Notifications from the plugin (main thread)
  virtual void onPluginChanged() = 0;
  virtual void onCrash() = 0;
  virtual void onEditorResize(int width, int height) = 0;
  virtual void onParamsChanged() = 0;
  virtual void onParamValue(uint32_t index) = 0;
  virtual void beforeUnload(HostedPlugin* p) = 0;

  // Host timer / file descriptor callbacks (CLAP timer-support, posix-fd-support): the event
  // loop of GUIs that cannot rely on the host's window messages (X11).
  virtual void onTimer(clap_id) {}
  virtual void onFd(int, clap_posix_fd_flags_t) {}
};

}  // namespace yov
