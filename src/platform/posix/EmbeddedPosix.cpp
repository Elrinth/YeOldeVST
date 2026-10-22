#include "platform/Embedded.h"

#include <cstdint>

#if defined(YOV_EMBED_HELPER)
// Defined in the generated embed_helper.S (see CMakeLists.txt).
extern "C" const uint8_t yov_helper_start[];
extern "C" const uint8_t yov_helper_end[];
#endif

namespace yov {

bool embeddedHelper(bool is32, const void*& data, size_t& size) {
#if defined(YOV_EMBED_HELPER)
  if (is32) return false;
  data = yov_helper_start;
  size = (size_t)(yov_helper_end - yov_helper_start);
  return size > 0;
#else
  (void)is32;
  (void)data;
  (void)size;
  return false;
#endif
}

}  // namespace yov
