// Fail-safe plugin scanner.
//
// Each plugin is probed in its own helper process (YeOldeVST-Host64/32.exe --probe) with a
// timeout, so a plugin that crashes or hangs only takes down that throwaway process. Probes run
// on a hidden desktop so nag screens and licence dialogs cannot pop up in front of the user
// (they show up as a timeout instead). Results go into the shared PluginDb, saved as we go.
#pragma once

#include "common/PluginDb.h"
#include "common/PeInfo.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yov {

// Locates the helper executables: next to this module first, then extracted from resources
// embedded in this module (the plugin DLL carries both helpers so it can ship as one file).
std::string helperExePath(PeArch arch);

struct ScanProgress {
  int total = 0;
  int done = 0;
  int ok = 0;
  int angry = 0;
  bool running = false;
  bool cancelled = false;
  std::vector<std::string> current;  // files being probed right now
  std::string message;
};

class Scanner {
 public:
  struct Options {
    std::vector<std::string> folders;
    std::vector<std::string> onlyFiles;  // if non-empty, (re)scan exactly these files
    bool rescanAll = false;              // ignore cached results
    bool retryFailed = false;            // re-probe crashed/timeout/failed entries
    bool testEditors = true;
    bool hiddenDesktop = true;
    int timeoutMs = 30000;
    int parallel = 4;
  };

  explicit Scanner(PluginDb& db) : db_(db) {}
  ~Scanner();

  // Starts scanning on a background thread. Returns false if a scan is already running here or
  // in another instance (a system-wide mutex prevents two scans fighting over the database).
  bool start(const Options& opt);
  void cancel();
  void wait();
  bool running() const { return running_.load(); }
  ScanProgress progress() const;

  // Called (from the scan thread) after each plugin finishes. Optional.
  std::function<void(const PluginRecord&)> onResult;

  // Probes one file synchronously (used by start() and by the CLI).
  // previous: what an earlier scan found in this file (editor results that passed are reused).
  static std::vector<PluginRecord> probeFile(const std::string& path, const Options& opt,
                                             std::atomic<bool>* cancel = nullptr,
                                             const std::vector<PluginRecord>* previous = nullptr);
  // Runs only the editor test for an already scanned plugin; updates r. False if cancelled.
  static bool testEditor(PluginRecord& r, const Options& opt, std::atomic<bool>* cancel = nullptr);

 private:
  void run(Options opt);

  PluginDb& db_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  mutable std::mutex m_;
  ScanProgress prog_;
};

// Recursively lists *.dll files below the folders (deduplicated, sorted).
std::vector<std::string> findPluginFiles(const std::vector<std::string>& folders);

}  // namespace yov
