// Loading plugin binaries: LoadLibrary on Windows, dlopen elsewhere.
#pragma once

#include <string>

namespace yov {

// Opens a plugin binary (DLL, .so, or a macOS .vst bundle folder). On failure returns null and
// a human readable reason. Does not show system error dialogs for missing dependencies.
void* dynOpen(const std::string& path, std::string& error);
void* dynSymbol(void* lib, const char* name);
void dynClose(void* lib);

}  // namespace yov
