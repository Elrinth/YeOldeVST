#include "plugin/HostedPlugin.h"

namespace yov {

std::unique_ptr<HostedPlugin> loadBridged(const std::string&, int32_t, bool, HostedPlugin::Listener*,
                                          std::string& error) {
  error = "the out-of-process bridge is not available yet";
  return nullptr;
}

}  // namespace yov
