#pragma once

#include <string>

namespace yov {

// Runs the out-of-process host side of a bridged plugin instance. `name` identifies the shared
// memory block created by the plugin. Returns the process exit code.
int runBridge(const std::string& name);

}  // namespace yov
