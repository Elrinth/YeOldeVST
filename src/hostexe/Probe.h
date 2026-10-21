#pragma once

#include <cstdint>
#include <string>

namespace yov {

struct ProbeOptions {
  std::string path;
  int32_t shellId = 0;
  bool testEditor = false;
  // Second scan phase: load, open/close the editor, unload. Runs on the user's desktop (editors
  // misbehave on a hidden one) after the first phase proved loading shows no dialog.
  bool editorOnly = false;
};

// Runs inside a throwaway helper process. Loads the plugin and exercises it step by step,
// printing "@@YOV ..." lines to stdout as it goes, so the parent can tell how far it got even
// when the process crashes or hangs. Returns the process exit code.
int runProbe(const ProbeOptions& opt);

}  // namespace yov
