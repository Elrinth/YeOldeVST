#include "platform/Embedded.h"

#include <windows.h>

namespace yov {

// Resource ids of the embedded helper executables (see embed_<config>.rc in CMakeLists.txt).
constexpr int kResHost64 = 201;
constexpr int kResHost32 = 202;

namespace {

HMODULE thisModule() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&thisModule), &mod);
  return mod;
}

}  // namespace

bool embeddedHelper(bool is32, const void*& data, size_t& size) {
  HMODULE mod = thisModule();
  HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(is32 ? kResHost32 : kResHost64), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
  if (!res) return false;
  HGLOBAL g = LoadResource(mod, res);
  size = SizeofResource(mod, res);
  data = g ? LockResource(g) : nullptr;
  return data && size;
}

}  // namespace yov
