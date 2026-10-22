// The window parts of platform/Helper.h for platforms without a backend yet (macOS: Cocoa is
// still to be written). Editors cannot be opened in the helper; everything else works.
#include "platform/Helper.h"

namespace yov::helper {

void init(bool) {}
void pumpMessages(int) {}
bool haveDisplay() { return false; }
void* containerCreate(int64_t, int, int) { return nullptr; }
void containerResize(void*, int, int) {}
bool containerChildSize(void*, int&, int&) { return false; }
void containerDestroy(void*) {}
void* probeWindowCreate() { return nullptr; }
void probeWindowDestroy(void*) {}
void startPopupEater(void (*)(const std::string&)) {}

}  // namespace yov::helper
