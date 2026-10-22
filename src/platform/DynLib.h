// Loading plugin binaries: LoadLibrary on Windows, dlopen elsewhere.
#pragma once

#include <string>

namespace yov {

// Opens a plugin binary (DLL, .so, or a macOS .vst bundle folder). On failure returns null and
// a human readable reason. Does not show system error dialogs for missing dependencies.
void* dynOpen(const std::string& path, std::string& error);
void* dynSymbol(void* lib, const char* name);
void dynClose(void* lib);

// Background threads running code of this module (the scanner) must not outlive it: a host may
// unload the plugin while one runs. Call dynPinSelf() before starting such a thread and
// dynReleaseSelf() as its very last call. On Windows the C runtime already pins the module for
// the thread's lifetime, so both do nothing. Elsewhere the module is marked never-unload
// (dlclose from code inside it is unsafe).
void* dynPinSelf();
void dynReleaseSelf(void* pin);

}  // namespace yov
