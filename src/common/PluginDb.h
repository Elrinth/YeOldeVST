// Scan results and user settings, stored as JSON in %APPDATA%\YeOldeVST so every instance (and
// every DAW) shares them.
#pragma once

#include "common/Json.h"
#include "common/Util.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace yov {

enum class ScanStatus {
  Unknown,      // never scanned
  Ok,           // loaded, processed audio, closed cleanly
  Warning,      // works, but something looked off (see warnings)
  Crashed,      // the probe process crashed (stage tells where)
  Timeout,      // the probe hung (often a hidden dialog or licence prompt)
  LoadFailed,   // DLL could not be loaded / entry point refused
  NotPlugin,    // a DLL without a VST2 entry point
  Unsupported,  // architecture we cannot host (ARM64 ...)
  NoHelper,     // needs a helper exe that is not available (e.g. 32-bit bridge)
  Shell,        // a shell container (WaveShell); its sub-plugins are separate records
};

const char* statusName(ScanStatus s);
ScanStatus statusFromName(const std::string& s);
// True for statuses that mean "you can load this".
inline bool statusLoadable(ScanStatus s) { return s == ScanStatus::Ok || s == ScanStatus::Warning; }
// True for statuses shown as problems.
inline bool statusAngry(ScanStatus s) {
  return s == ScanStatus::Crashed || s == ScanStatus::Timeout || s == ScanStatus::LoadFailed;
}

struct PluginRecord {
  std::string path;
  int32_t shellId = 0;
  FileStamp stamp;
  std::string arch;  // "x86" / "x64"

  ScanStatus status = ScanStatus::Unknown;
  std::string detail;  // human readable reason for the status
  std::string stage;   // probe stage that crashed / hung
  std::vector<std::string> warnings;

  std::string name;  // effect name, or product name, or file name
  std::string vendor;
  std::string product;
  int32_t uniqueId = 0;
  int32_t version = 0;
  int32_t vstVersion = 0;
  int32_t category = 0;
  bool isSynth = false;
  bool hasEditor = false;
  bool usesChunks = false;
  bool midiInput = false;
  int32_t numInputs = 0;
  int32_t numOutputs = 0;
  int32_t numParams = 0;
  int32_t numPrograms = 0;
  double cpuPercent = 0;  // processing cost measured during the scan
  uint64_t scannedAt = 0;  // unix seconds
  uint32_t scanMs = 0;

  std::string displayName() const;
  std::string key() const;  // path|shellId, lower-cased
  bool isInstrument() const { return isSynth || category == 2 || category == 11; }

  Json toJson() const;
  static PluginRecord fromJson(const Json& j);
};

struct Settings {
  std::vector<std::string> folders;
  bool testEditors = true;
  bool hiddenDesktop = true;
  int timeoutSec = 30;
  int parallel = 4;

  static std::string filePath();
  void load();
  bool save() const;
};

// Thread-safe container for scan results.
class PluginDb {
 public:
  static std::string defaultPath();
  const std::string& filePath() const { return path_; }
  void setFilePath(std::string p) { path_ = std::move(p); }

  bool load();   // from disk (replaces contents)
  bool save();   // to disk
  // Reloads from disk if another instance saved since we last loaded/saved.
  bool reloadIfChanged();

  std::vector<PluginRecord> all() const;
  bool find(const std::string& path, int32_t shellId, PluginRecord& out) const;
  // Fallback lookup for state recall when a plugin moved: by unique id + name.
  bool findMoved(int32_t uniqueId, const std::string& name, PluginRecord& out) const;
  void upsert(const PluginRecord& r);
  void removeShellChildren(const std::string& path);
  // Drop records for files that are no longer inside any of the folders, or no longer exist.
  void prune(const std::vector<std::string>& folders);
  void clear();

 private:
  mutable std::mutex m_;
  std::string path_ = defaultPath();
  std::vector<PluginRecord> recs_;
  FileStamp diskStamp_;
};

}  // namespace yov
