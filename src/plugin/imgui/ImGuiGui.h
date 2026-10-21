// The Dear ImGui plugin window (Linux, macOS; on Windows behind YEOLDEVST_GUI=imgui).
#pragma once

#include "plugin/Gui.h"

#include <memory>

namespace yov {

std::unique_ptr<Gui> makeImGuiGui(Plugin& plugin);
void imguiGuiShutdown();

}  // namespace yov
