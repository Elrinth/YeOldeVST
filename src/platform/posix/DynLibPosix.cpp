#include "platform/DynLib.h"

#include "common/ModuleInfo.h"
#include "common/Util.h"

#include <dlfcn.h>

#include <cstring>

namespace yov {

void* dynOpen(const std::string& pathIn, std::string& error) {
  std::string path = pathIn;
#ifdef __APPLE__
  if (dirExists(path)) {
    // A .vst bundle: load its executable (Contents/MacOS/<name>).
    std::vector<DirEntry> list;
    std::string macos = joinPath(joinPath(path, "Contents"), "MacOS");
    if (listDir(macos, list))
      for (auto& e : list)
        if (!e.isDir) {
          path = joinPath(macos, e.name);
          break;
        }
  }
#endif
  // RTLD_LOCAL: plugins built from the same framework must not share each other's symbols.
  void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char* e = dlerror();
    std::string why = e ? e : "unknown error";
    ModuleInfo mi = readModuleInfo(pathIn);
    if (mi.valid && mi.format != nativeFormat())
      error = std::string("this is a ") + formatName(mi.format) + " plugin, not a " + formatName(nativeFormat()) +
              " one";
    else if (mi.valid && mi.arch != nativeArch())
      error = std::string("wrong architecture (") + archName(mi.arch) + " plugin, " + archName(nativeArch()) +
              " host)";
    else if (why.find("cannot open shared object file") != std::string::npos && why.rfind(path, 0) != 0)
      error = "a library this plugin needs is missing (" + why + ")";
    else
      error = "dlopen failed: " + why;
  }
  return h;
}

void* dynSymbol(void* lib, const char* name) { return dlsym(lib, name); }

void dynClose(void* lib) { dlclose(lib); }

}  // namespace yov
