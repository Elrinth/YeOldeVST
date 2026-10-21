// Reads a DLL's PE headers from disk (never loads it) to learn its CPU architecture and which
// plugin entry points it exports. This lets the scanner classify files safely before any
// plugin code runs.
#pragma once

#include <string>
#include <vector>

namespace yov {

enum class PeArch { Unknown, X86, X64, Arm64, Other };

struct PeInfo {
  bool valid = false;  // parsed as a PE DLL
  bool isDll = false;
  PeArch arch = PeArch::Unknown;
  bool hasVstPluginMain = false;  // "VSTPluginMain"
  bool hasMain = false;           // "main" / "main_plugin" / "_main" (VST 1.x / early 2.x)
  bool hasVst3Factory = false;    // "GetPluginFactory"
  bool hasClapEntry = false;      // "clap_entry"
  std::string error;

  bool looksLikeVst2() const { return hasVstPluginMain || hasMain; }
};

PeInfo readPeInfo(const std::string& path);
const char* archName(PeArch a);

}  // namespace yov
