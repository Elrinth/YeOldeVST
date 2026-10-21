#include "plugin/Gui.h"

#include "common/Util.h"

#if defined(_WIN32)
#include "plugin/win/Win32Gui.h"
#endif
#if defined(YOV_IMGUI)
#include "plugin/imgui/ImGuiGui.h"
#endif

namespace yov {

const char* Gui::windowApi() {
#if defined(_WIN32)
  return CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
  return CLAP_WINDOW_API_COCOA;
#else
  return CLAP_WINDOW_API_X11;
#endif
}

std::unique_ptr<Gui> Gui::create(Plugin& plugin) {
#if defined(_WIN32)
#if defined(YOV_IMGUI)
  std::string which;
  if (getEnv("YEOLDEVST_GUI", which) && which == "imgui") return makeImGuiGui(plugin);
#endif
  return std::make_unique<Win32Gui>(plugin);
#elif defined(YOV_IMGUI)
  return makeImGuiGui(plugin);
#else
  (void)plugin;
  return nullptr;
#endif
}

void Gui::shutdown() {
#if defined(_WIN32)
  Win32Gui::unregisterClasses();
#endif
#if defined(YOV_IMGUI)
  imguiGuiShutdown();
#endif
}

}  // namespace yov
