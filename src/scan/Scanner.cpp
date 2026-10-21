#include "scan/Scanner.h"

#include "common/Seh.h"
#include "common/Util.h"
#include "platform/Embedded.h"
#include "platform/Process.h"
#include "vst2/aeffect.h"

#include <algorithm>
#include <ctime>
#include <set>

namespace yov {

namespace {

std::string extractHelper(bool is32, const char* fileName) {
  const void* data = nullptr;
  size_t size = 0;
  if (!embeddedHelper(is32, data, size)) return {};
  // One folder per build of the plugin, so different versions never overwrite each other's
  // helper while it is running.
  FileStamp st;
  fileStamp(thisModulePath(), st);
  char tag[64];
  snprintf(tag, sizeof tag, "%llx-%llx", (unsigned long long)st.size, (unsigned long long)st.mtime);
  auto dir = joinPath(joinPath(localAppDataDir(), "bin"), tag);
  makeDirs(dir);
  auto path = joinPath(dir, fileName);
  FileStamp have;
  if (fileStamp(path, have) && have.size == size) return path;
  // New build: remove helpers left by older builds (files still running stay; Windows refuses).
  static bool cleaned = false;
  if (!cleaned) {
    cleaned = true;
    auto bin = joinPath(localAppDataDir(), "bin");
    std::vector<DirEntry> list;
    if (listDir(bin, list)) {
      for (auto& e : list) {
        if (!e.isDir || e.name == tag) continue;
        auto old = joinPath(bin, e.name);
        for (const char* f : {"YeOldeVST-Host32.exe", "YeOldeVST-Host64.exe", "YeOldeVST-Host32", "YeOldeVST-Host64"})
          removeFile(joinPath(old, f));
        removeDir(old);
      }
    }
  }
  if (!writeFileAtomic(path, std::string_view((const char*)data, size), true)) return {};
  return path;
}

std::string helperName(Arch arch) {
#ifdef _WIN32
  return arch == Arch::X86 ? "YeOldeVST-Host32.exe" : "YeOldeVST-Host64.exe";
#else
  return arch == Arch::X86 ? "YeOldeVST-Host32" : "YeOldeVST-Host64";
#endif
}

// Plugin binaries this platform can host: DLLs on Windows, shared objects on Linux, .vst
// bundles on macOS.
bool isPluginFile(const std::string& name) {
#if defined(_WIN32)
  return endsWithNoCase(name, ".dll");
#elif defined(__APPLE__)
  return endsWithNoCase(name, ".vst") || endsWithNoCase(name, ".dylib");
#else
  return endsWithNoCase(name, ".so");
#endif
}

}  // namespace

Arch archFromName(const std::string& name) {
  if (name == "x86") return Arch::X86;
  if (name == "x64") return Arch::X64;
  if (name == "arm64") return Arch::Arm64;
  return name.empty() ? Arch::Unknown : Arch::Other;
}

std::string helperExePath(Arch arch) {
  if (arch != Arch::X86 && arch != nativeArch()) return {};
  static std::mutex m;
  static std::string cache[2];
  int slot = arch == Arch::X86 ? 0 : 1;
  std::lock_guard<std::mutex> lk(m);
  if (!cache[slot].empty() && fileExists(cache[slot])) return cache[slot];
  auto beside = joinPath(thisModuleDir(), helperName(arch));
  if (fileExists(beside)) return cache[slot] = beside;
  auto extracted = extractHelper(arch == Arch::X86, helperName(arch).c_str());
  return cache[slot] = extracted;
}

std::vector<std::string> findPluginFiles(const std::vector<std::string>& folders) {
  std::set<std::string> seen;
  std::vector<std::string> out;
  std::vector<std::string> stack;
  for (auto& f : folders)
    if (!f.empty()) stack.push_back(f);
  int dirs = 0;
  std::vector<DirEntry> list;
  while (!stack.empty() && dirs < 100000) {
    std::string dir = stack.back();
    stack.pop_back();
    ++dirs;
    if (!listDir(dir, list)) continue;
    for (auto& e : list) {
      std::string full = joinPath(dir, e.name);
      if (e.isDir) {
        // Don't follow junctions/symlinks (loops); skip VST3/CLAP bundles, they are not VST2.
        if (e.isLink) continue;
#ifdef __APPLE__
        if (endsWithNoCase(e.name, ".vst")) {
          if (seen.insert(pathKey(full)).second) out.push_back(full);
          continue;
        }
#endif
        if (endsWithNoCase(e.name, ".vst3") || endsWithNoCase(e.name, ".clap") || endsWithNoCase(e.name, ".bundle"))
          continue;
#ifndef _WIN32
        if (endsWithNoCase(e.name, ".lv2")) continue;  // LV2 bundles hold .so files that are not VST2
#endif
        stack.push_back(full);
      } else {
        if (!isPluginFile(e.name)) continue;
        if (seen.insert(pathKey(full)).second) out.push_back(full);
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return foldCase(a) < foldCase(b);
  });
  return out;
}

// ---------------------------------------------------------------------------------------------
// Running one probe

namespace {

struct ProbeOutcome {
  std::vector<std::pair<std::string, std::string>> lines;  // kind, payload
  bool started = false;
  bool timedOut = false;
  bool cancelled = false;
  uint32_t ms = 0;
  std::string exitText;  // how the process ended ("access violation (exit code 0xC0000005)")
  std::string spawnError;
};

enum class Phase { All, NoEditor, EditorOnly };

ProbeOutcome runHelper(const std::string& exe, const std::string& path, int32_t shellId, const Scanner::Options& opt,
                       Phase phase, std::atomic<bool>* cancel, bool forceHidden = false) {
  ProbeOutcome o;
  std::vector<std::string> args = {"--probe", path};
  if (shellId) {
    args.push_back("--shell-id");
    args.push_back(std::to_string(shellId));
  }
  if (phase == Phase::All) args.push_back("--editor");
  if (phase == Phase::EditorOnly) args.push_back("--editor-only");

  SpawnOptions so;
  so.cwd = dirOf(path);
  so.captureOutput = true;
  so.group = SpawnOptions::Group::Scan;
  // Editors misbehave on a desktop nobody can see, so the editor phase runs on the user's one
  // (its window is placed off-screen).
  so.hiddenDesktop = opt.hiddenDesktop && (phase != Phase::EditorOnly || forceHidden);
  ChildProcess proc;
  if (!proc.start(exe, args, so, o.spawnError)) return o;
  o.started = true;

  std::string buf;
  auto t0 = nowMs();
  for (;;) {
    bool ended = proc.wait(25);
    proc.readOutput(buf);
    if (ended) break;
    if (cancel && cancel->load()) {
      proc.kill(0xC000013A);
      proc.wait(2000);
      o.cancelled = true;
      break;
    }
    if (nowMs() - t0 > (uint64_t)opt.timeoutMs) {
      proc.kill(0x102 /*WAIT_TIMEOUT*/);
      proc.wait(2000);
      o.timedOut = true;
      break;
    }
  }
  proc.readOutput(buf);
  o.exitText = proc.exitDescription();
  o.ms = (uint32_t)(nowMs() - t0);

  size_t pos = 0;
  while (pos < buf.size()) {
    size_t nl = buf.find('\n', pos);
    std::string line = buf.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? buf.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // Plugins may print their own junk; our lines can appear mid-line after it.
    auto at = line.find("@@YOV ");
    if (at == std::string::npos) continue;
    line = line.substr(at + 6);
    auto sp = line.find(' ');
    o.lines.emplace_back(line.substr(0, sp), sp == std::string::npos ? "" : line.substr(sp + 1));
  }
  return o;
}

const char* stageText(const std::string& s) {
  if (s == "load") return "while loading";
  if (s == "info") return "while being asked for its name/info";
  if (s == "setup") return "while being switched on";
  if (s == "process") return "while processing audio";
  if (s == "params") return "while listing parameters";
  if (s == "programs") return "while listing presets";
  if (s == "state") return "while saving/restoring its state";
  if (s == "editor") return "while opening its editor";
  if (s == "editor-close") return "while closing its editor";
  if (s == "suspend") return "while being switched off";
  if (s == "close") return "while being unloaded";
  if (s == "shell") return "while listing its sub-plugins";
  return "during the test";
}

void classify(PluginRecord& r, const ProbeOutcome& o, std::vector<std::pair<int32_t, std::string>>* shellKids) {
  std::string lastStage = "start", error, faultStage, faultText;
  bool done = false;
  r.warnings.clear();
  for (auto& [kind, payload] : o.lines) {
    if (kind == "stage") lastStage = payload;
    else if (kind == "error") error = payload;
    else if (kind == "warn") r.warnings.push_back(payload);
    else if (kind == "popup") r.warnings.push_back("showed a popup: " + payload);
    else if (kind == "done") done = true;
    else if (kind == "arch") r.arch = payload;
    else if (kind == "fault") {
      auto tab = payload.find('\t');
      faultStage = payload.substr(0, tab);
      faultText = tab == std::string::npos ? "" : payload.substr(tab + 1);
    } else if (kind == "shell" && shellKids) {
      auto tab = payload.find('\t');
      shellKids->emplace_back((int32_t)std::strtol(payload.substr(0, tab).c_str(), nullptr, 10),
                              tab == std::string::npos ? "" : payload.substr(tab + 1));
    } else if (kind == "info") {
      auto tab = payload.find('\t');
      auto key = payload.substr(0, tab);
      auto val = tab == std::string::npos ? std::string() : payload.substr(tab + 1);
      long long iv = std::strtoll(val.c_str(), nullptr, 10);
      if (key == "name") r.name = val;
      else if (key == "product") r.product = val;
      else if (key == "vendor") r.vendor = val;
      else if (key == "vstVersion") r.vstVersion = (int32_t)iv;
      else if (key == "category") r.category = (int32_t)iv;
      else if (key == "uniqueId") r.uniqueId = (int32_t)iv;
      else if (key == "version") r.version = (int32_t)iv;
      else if (key == "flags") {
        r.isSynth = (iv & vst2::effFlagsIsSynth) != 0;
        r.hasEditor = (iv & vst2::effFlagsHasEditor) != 0;
        r.usesChunks = (iv & vst2::effFlagsProgramChunks) != 0;
      } else if (key == "numInputs") r.numInputs = (int32_t)iv;
      else if (key == "numOutputs") r.numOutputs = (int32_t)iv;
      else if (key == "numParams") r.numParams = (int32_t)iv;
      else if (key == "numPrograms") r.numPrograms = (int32_t)iv;
      else if (key == "midiInput") r.midiInput = iv != 0;
      else if (key == "cpuPercent") r.cpuPercent = std::strtod(val.c_str(), nullptr);
    }
  }
  r.scanMs = o.ms;
  r.scannedAt = (uint64_t)time(nullptr);
  r.stage.clear();
  r.detail.clear();
  bool gotFlags = false, sawEditor = false;
  for (auto& [kind, payload] : o.lines) {
    if (kind == "info" && payload.rfind("flags\t", 0) == 0) gotFlags = true;
    if (kind == "stage" && (payload == "editor" || payload == "editor-close")) sawEditor = true;
  }
  if (gotFlags && !r.hasEditor) r.editor = "none";
  else if (sawEditor) r.editor = done ? "ok" : o.timedOut ? "hangs" : "crashed";

  if (!o.spawnError.empty()) {
    r.status = ScanStatus::NoHelper;
    r.detail = o.spawnError;
  } else if (!error.empty()) {
    r.status = ScanStatus::LoadFailed;
    r.detail = error;
    for (auto& w : r.warnings)
      if (w.rfind("showed a popup", 0) == 0) r.detail += " (after it " + w + ")";
  } else if (done) {
    if (shellKids && !shellKids->empty()) {
      r.status = ScanStatus::Shell;
      r.detail = "shell with " + std::to_string(shellKids->size()) + " plugins";
    } else {
      r.status = r.warnings.empty() ? ScanStatus::Ok : ScanStatus::Warning;
      if (!r.warnings.empty()) r.detail = r.warnings.front();
    }
  } else if (o.timedOut) {
    r.status = ScanStatus::Timeout;
    r.stage = lastStage;
    r.detail = std::string("hung ") + stageText(lastStage) + " (killed after " +
               std::to_string(o.ms / 1000) + " s; maybe a hidden dialog or licence prompt)";
  } else {
    std::string stg = faultStage.empty() ? lastStage : faultStage;
    std::string why = faultText.empty() ? o.exitText : faultText;
    r.stage = stg;
    // Map the guard's stage names (effEditOpen ...) to probe stages for the message.
    std::string probeStage = lastStage;
    if (stg == "close" || probeStage == "close") {
      r.status = ScanStatus::Warning;
      r.warnings.push_back("crashes when unloaded: " + why);
      r.detail = r.warnings.back();
    } else {
      r.status = ScanStatus::Crashed;
      r.detail = std::string("crashed ") + stageText(probeStage) + ": " + why;
    }
  }
}

// Second phase: the editor test, in a fresh process on the visible desktop. Only runs when the
// first phase passed, so we know loading the plugin does not pop up anything.
void runEditorPhase(PluginRecord& r, const std::string& exe, const Scanner::Options& opt, std::atomic<bool>* cancel) {
  // Editors that already passed (this run or an earlier scan of the same file) are not tested again.
  if (!opt.testEditors || !r.hasEditor || !statusLoadable(r.status) || r.editor == "ok" || r.editor == "none") return;
  // A plugin that popped something up while loading would do it again: keep that hidden too.
  bool popups = false;
  for (auto& w : r.warnings)
    if (w.rfind("showed a popup", 0) == 0) popups = true;
  auto o = runHelper(exe, r.path, r.shellId, opt, Phase::EditorOnly, cancel, popups);
  if (o.cancelled) return;
  r.scanMs += o.ms;
  PluginRecord e = r;
  classify(e, o, nullptr);
  r.editor = e.status == ScanStatus::Ok || e.status == ScanStatus::Warning ? "ok"
             : e.status == ScanStatus::Timeout                             ? "hangs"
             : e.status == ScanStatus::LoadFailed                          ? "failed"
                                                                           : "crashed";
  if (e.status == ScanStatus::Ok || e.status == ScanStatus::Warning) {
    for (auto& w : e.warnings)
      if (std::find(r.warnings.begin(), r.warnings.end(), w) == r.warnings.end()) r.warnings.push_back(w);
    if (!r.warnings.empty() && r.status == ScanStatus::Ok) {
      r.status = ScanStatus::Warning;
      r.detail = r.warnings.front();
    }
  } else if (e.status == ScanStatus::LoadFailed) {
    r.warnings.push_back("failed to load a second time: " + e.detail);
    r.status = ScanStatus::Warning;
    r.detail = r.warnings.back();
  } else {
    // Crashed / hung opening the editor. Audio may still work (generic editor).
    r.status = e.status;
    r.stage = e.stage;
    r.detail = e.detail + " (audio processing passed; the generic editor may still work)";
  }
}

}  // namespace

bool Scanner::testEditor(PluginRecord& r, const Options& opt, std::atomic<bool>* cancel) {
  std::string exe = helperExePath(r.arch == "x86" ? Arch::X86 : nativeArch());
  if (exe.empty()) return false;
  Options o = opt;
  o.testEditors = true;
  runEditorPhase(r, exe, o, cancel);
  return !(cancel && cancel->load());
}

std::vector<PluginRecord> Scanner::probeFile(const std::string& path, const Options& opt, std::atomic<bool>* cancel,
                                             const std::vector<PluginRecord>* previous) {
  std::vector<PluginRecord> out;
  PluginRecord r;
  r.path = path;
  fileStamp(path, r.stamp);
  r.scannedAt = (uint64_t)time(nullptr);
  ModuleInfo pe = readModuleInfo(path);
  r.arch = archName(pe.arch);
  if (pe.valid && pe.format != nativeFormat()) {
    r.status = ScanStatus::Unsupported;
    r.detail = std::string(formatName(pe.format)) + " plugin; this build of YeOldeVST hosts " +
               formatName(nativeFormat()) + " plugins";
    out.push_back(r);
    return out;
  }
  if (!pe.valid || !pe.isLibrary) {
    r.status = ScanStatus::NotPlugin;
#ifdef _WIN32
    r.detail = pe.error.empty() ? "not a DLL" : pe.error;
#else
    r.detail = pe.error.empty() ? "not a shared library" : pe.error;
#endif
    out.push_back(r);
    return out;
  }
  if (!pe.looksLikeVst2()) {
    r.status = ScanStatus::NotPlugin;
    r.detail = pe.hasVst3Factory ? "VST3 plugin; load it directly in your DAW"
               : pe.hasClapEntry ? "CLAP plugin; load it directly in your DAW"
                                 : "no VST2 entry point (probably a support library)";
    out.push_back(r);
    return out;
  }
  if (pe.arch != nativeArch() && pe.arch != Arch::X86) {
    r.status = ScanStatus::Unsupported;
    r.detail = std::string(archName(pe.arch)) + " plugins are not supported";
    out.push_back(r);
    return out;
  }
  std::string exe = helperExePath(pe.arch);
  if (exe.empty()) {
    r.status = ScanStatus::NoHelper;
    r.detail = pe.arch == Arch::X86 ? "32-bit plugin, but the 32-bit helper (" + helperName(Arch::X86) + ") is missing"
                                    : "the 64-bit helper (" + helperName(pe.arch) + ") is missing";
    out.push_back(r);
    return out;
  }

  // Editor results of an earlier scan of this very file are kept when the editor isn't re-tested.
  auto previousEditor = [&](int32_t shellId) -> std::string {
    if (!previous) return {};
    for (auto& p : *previous)
      if (p.shellId == shellId && p.stamp == r.stamp) return p.editor;
    return {};
  };
  auto inherit = [&](PluginRecord& rec) {
    if (!rec.editor.empty() || !rec.hasEditor || !statusLoadable(rec.status)) return;
    std::string prev = previousEditor(rec.shellId);
    if (prev == "ok" || !opt.testEditors) rec.editor = prev;
  };
  const bool editorKnownOk = previousEditor(0) == "ok";
  const Phase first = !opt.testEditors || opt.hiddenDesktop || editorKnownOk ? Phase::NoEditor : Phase::All;
  std::vector<std::pair<int32_t, std::string>> kids;
  auto o = runHelper(exe, path, 0, opt, first, cancel);
  if (o.cancelled) return {};
  classify(r, o, &kids);
  if (r.name.empty()) r.name = r.product;
  inherit(r);
  if (r.status != ScanStatus::Shell) runEditorPhase(r, exe, opt, cancel);
  if (cancel && cancel->load()) return {};
  out.push_back(r);

  for (auto& [id, kidName] : kids) {
    if (cancel && cancel->load()) return {};
    PluginRecord k;
    k.path = path;
    k.shellId = id;
    k.stamp = r.stamp;
    k.arch = r.arch;
    auto ko = runHelper(exe, path, id, opt, first, cancel);
    if (ko.cancelled) return {};
    classify(k, ko, nullptr);
    if (k.name.empty()) k.name = kidName;
    inherit(k);
    runEditorPhase(k, exe, opt, cancel);
    out.push_back(k);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------

Scanner::~Scanner() {
  cancel();
  wait();
}

bool Scanner::start(const Options& opt) {
  if (running_.load()) return false;
  wait();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    prog_ = ScanProgress{};
    prog_.running = true;
    prog_.message = "Looking for plugins...";
  }
  running_ = true;
  thread_ = std::thread([this, opt] { run(opt); });
  return true;
}

void Scanner::cancel() { cancel_ = true; }

void Scanner::wait() {
  if (thread_.joinable()) thread_.join();
}

ScanProgress Scanner::progress() const {
  std::lock_guard<std::mutex> lk(m_);
  return prog_;
}

void Scanner::run(Options opt) {
  // One scan at a time across all instances / DAWs on this machine.
  SystemLock lock;
  if (!lock.tryAcquire("YeOldeVST-ScanLock")) {
    std::lock_guard<std::mutex> lk(m_);
    prog_.running = false;
    prog_.message = "Another YeOldeVST instance is scanning right now. Try again when it is done.";
    running_ = false;
    return;
  }

  // Without a display there is nothing to open editors on.
  if (!haveDesktop()) opt.testEditors = false;
  if (opt.hiddenDesktop) hiddenDesktopAcquire();
  struct HiddenRelease {
    bool on;
    ~HiddenRelease() {
      if (on) hiddenDesktopRelease();
    }
  } hiddenRelease{opt.hiddenDesktop};

  db_.reloadIfChanged();
  std::vector<std::string> files = opt.onlyFiles.empty() ? findPluginFiles(opt.folders) : opt.onlyFiles;

  // Decide what needs probing: a full test, or (for unchanged plugins whose editor was never
  // tested) just the editor test.
  struct Job {
    std::string path;
    bool editorOnly;
  };
  std::vector<Job> jobs;
  bool host32 = !helperExePath(Arch::X86).empty();
  for (auto& f : files) {
    PluginRecord old;
    FileStamp st;
    fileStamp(f, st);
    bool have = db_.find(f, 0, old);
    bool needs = !have || opt.rescanAll || !opt.onlyFiles.empty() || !(old.stamp == st) ||
                 old.status == ScanStatus::Unknown || (old.status == ScanStatus::NoHelper && host32) ||
                 (opt.retryFailed && statusAngry(old.status));
    if (needs) {
      jobs.push_back({f, false});
    } else if (opt.testEditors) {
      for (auto& r : db_.forPath(f))
        if (r.hasEditor && statusLoadable(r.status) && r.editor.empty()) {
          jobs.push_back({f, true});
          break;
        }
    }
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    prog_.total = (int)jobs.size();
    prog_.message = jobs.empty() ? "Everything is up to date." : "Scanning...";
  }
  logLine("scan: %d files found, %d to probe", (int)files.size(), (int)jobs.size());

  std::atomic<size_t> next{0};
  std::mutex saveM;
  uint64_t lastSave = nowMs();
  auto worker = [&] {
    for (;;) {
      if (cancel_.load()) return;
      size_t i = next.fetch_add(1);
      if (i >= jobs.size()) return;
      const std::string& f = jobs[i].path;
      {
        std::lock_guard<std::mutex> lk(m_);
        prog_.current.push_back(f);
      }
      auto previous = db_.forPath(f);
      std::vector<PluginRecord> recs;
      if (jobs[i].editorOnly) {
        for (auto& r : previous) {
          if (!r.hasEditor || !statusLoadable(r.status) || !r.editor.empty()) continue;
          if (!testEditor(r, opt, &cancel_)) break;
          recs.push_back(r);
        }
        if (cancel_.load()) recs.clear();
        for (auto& r : recs) {
          db_.upsert(r);
          if (onResult) onResult(r);
        }
      } else {
        recs = probeFile(f, opt, &cancel_, &previous);
        if (!recs.empty()) {
          db_.removeShellChildren(f);
          for (auto& r : recs) {
            db_.upsert(r);
            if (onResult) onResult(r);
          }
        }
      }
      {
        std::lock_guard<std::mutex> lk(m_);
        prog_.current.erase(std::find(prog_.current.begin(), prog_.current.end(), f));
        if (!recs.empty()) {
          ++prog_.done;
          for (auto& r : recs) {
            if (statusLoadable(r.status)) ++prog_.ok;
            if (statusAngry(r.status)) ++prog_.angry;
          }
        }
      }
      std::lock_guard<std::mutex> lk(saveM);
      if (nowMs() - lastSave > 3000) {
        db_.save();  // keep progress if the DAW dies mid-scan
        lastSave = nowMs();
      }
    }
  };
  int n = std::clamp(opt.parallel, 1, 16);
  std::vector<std::thread> pool;
  for (int i = 0; i < n; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  if (opt.onlyFiles.empty() && !cancel_.load()) db_.prune(opt.folders);
  db_.save();
  lock.release();
  {
    std::lock_guard<std::mutex> lk(m_);
    prog_.running = false;
    prog_.cancelled = cancel_.load();
    prog_.current.clear();
    char msg[256];
    snprintf(msg, sizeof msg, "%s %d of %d scanned: %d OK, %d with problems.",
             prog_.cancelled ? "Cancelled." : "Done.", prog_.done, prog_.total, prog_.ok, prog_.angry);
    prog_.message = msg;
  }
  logLine("scan finished");
  running_ = false;
}

}  // namespace yov
