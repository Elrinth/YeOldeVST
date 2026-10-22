#include "platform/DynLib.h"

#include "common/Util.h"

#include <windows.h>

namespace yov {

namespace {

std::string lastErrorText(DWORD err) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 err, 0, (LPWSTR)&msg, 0, nullptr);
  std::string s = msg ? trim(narrow(msg)) : "error " + std::to_string(err);
  LocalFree(msg);
  return s;
}

}  // namespace

void* dynOpen(const std::string& path, std::string& error) {
  // Don't let a missing dependency pop up a system error box.
  UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  HMODULE mod = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  DWORD err = GetLastError();
  SetErrorMode(oldMode);
  if (!mod) {
    if (err == ERROR_MOD_NOT_FOUND)
      error = "a DLL this plugin needs is missing (" + lastErrorText(err) + ")";
    else if (err == ERROR_BAD_EXE_FORMAT)
      error = "wrong architecture (32-bit vs 64-bit)";
    else
      error = "LoadLibrary failed: " + lastErrorText(err);
  }
  return mod;
}

void* dynSymbol(void* lib, const char* name) { return (void*)GetProcAddress((HMODULE)lib, name); }

void dynClose(void* lib) { FreeLibrary((HMODULE)lib); }

// Nothing to do on Windows: threads started through the C runtime (std::thread, _beginthreadex)
// already hold a reference on the module of their start routine and drop it when they exit.
// Ending the thread here with FreeLibraryAndExitThread would skip that release and keep the
// module loaded forever.
void* dynPinSelf() { return nullptr; }

void dynReleaseSelf(void*) {}

}  // namespace yov
