// Helper executables embedded in the plugin binary, so it can be installed as a single file.
// Windows: RCDATA resources. Linux: linked in with .incbin.
#pragma once

#include <cstddef>

namespace yov {

// is32: the 32-bit helper (Windows) instead of the native one.
bool embeddedHelper(bool is32, const void*& data, size_t& size);

}  // namespace yov
