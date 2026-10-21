// Reads a plugin binary's headers from disk (never loads it) to learn its format, CPU
// architecture and which plugin entry points it exports. This lets the scanner classify files
// safely before any plugin code runs. Understands Windows DLLs (PE), Linux shared objects (ELF)
// and macOS bundles / dylibs (Mach-O, including universal binaries).
#pragma once

#include <string>
#include <vector>

namespace yov {

enum class Arch { Unknown, X86, X64, Arm64, Other };
enum class BinaryFormat { Unknown, PE, ELF, MachO };

struct ModuleInfo {
  bool valid = false;      // parsed as one of the formats above
  bool isLibrary = false;  // a DLL / shared object / bundle, not an executable
  BinaryFormat format = BinaryFormat::Unknown;
  Arch arch = Arch::Unknown;
  std::vector<Arch> archs;        // Mach-O universal binaries carry several
  bool hasVstPluginMain = false;  // "VSTPluginMain"
  bool hasMain = false;           // "main" / "main_plugin" / "_main" (VST 1.x / early 2.x)
  bool hasVst3Factory = false;    // "GetPluginFactory"
  bool hasClapEntry = false;      // "clap_entry"
  std::string error;

  bool looksLikeVst2() const { return hasVstPluginMain || hasMain; }
};

// path: the DLL / .so, or a macOS .vst bundle folder (its executable is read).
ModuleInfo readModuleInfo(const std::string& path);
const char* archName(Arch a);
const char* formatName(BinaryFormat f);

// The binary format and architecture this build hosts natively.
BinaryFormat nativeFormat();
Arch nativeArch();

}  // namespace yov
