#include "scan/Scanner.h"

#include "common/Seh.h"
#include "common/Util.h"
#include "vst2/aeffect.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <set>

namespace yov {

// Resource ids of the embedded helper executables (see resources/embed.rc.in).
constexpr int kResHost64 = 201;
constexpr int kResHost32 = 202;

namespace {

HMODULE thisModule() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&thisModule), &mod);
  return mod;
}

std::string extractResource(int id, const char* fileName) {
  HMODULE mod = thisModule();
  HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
  if (!res) return {};
  HGLOBAL g = LoadResource(mod, res);
  DWORD size = SizeofResource(mod, res);
  const void* data = g ? LockResource(g) : nullptr;
  if (!data || !size) return {};
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
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(joinPath(bin, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::string name = narrow(fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == "." || name == ".." || name == tag) continue;
        auto old = joinPath(bin, name);
        for (const char* f : {"YeOldeVST-Host32.exe", "YeOldeVST-Host64.exe"}) DeleteFileW(widen(joinPath(old, f)).c_str());
        RemoveDirectoryW(widen(old).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  if (!writeFileAtomic(path, std::string_view((const char*)data, size))) return {};
  return path;
}

std::string helperName(PeArch arch) {
  return arch == PeArch::X86 ? "YeOldeVST-Host32.exe" : "YeOldeVST-Host64.exe";
}

}  // namespace

std::string helperExePath(PeArch arch) {
  if (arch != PeArch::X86 && arch != PeArch::X64) return {};
  static std::mutex m;
  static std::string cache[2];
  int slot = arch == PeArch::X86 ? 0 : 1;
  std::lock_guard<std::mutex> lk(m);
  if (!cache[slot].empty() && fileExists(cache[slot])) return cache[slot];
  auto beside = joinPath(thisModuleDir(), helperName(arch));
  if (fileExists(beside)) return cache[slot] = beside;
  auto extracted = extractResource(arch == PeArch::X86 ? kResHost32 : kResHost64, helperName(arch).c_str());
  return cache[slot] = extracted;
}

std::vector<std::string> findPluginFiles(const std::vector<std::string>& folders) {
  std::set<std::string> seen;
  std::vector<std::string> out;
  std::vector<std::wstring> stack;
  for (auto& f : folders)
    if (!f.empty()) stack.push_back(widen(f));
  int dirs = 0;
  while (!stack.empty() && dirs < 100000) {
    std::wstring dir = stack.back();
    stack.pop_back();
    ++dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      std::wstring full = dir + L"\\" + name;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // Don't follow junctions/symlinks (loops); skip VST3/CLAP bundles, they are not VST2.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        auto n = narrow(name);
        if (endsWithNoCase(n, ".vst3") || endsWithNoCase(n, ".clap") || endsWithNoCase(n, ".bundle")) continue;
        stack.push_back(full);
      } else {
        auto n = narrow(name);
        if (!endsWithNoCase(n, ".dll")) continue;
        auto f8 = narrow(full);
        if (seen.insert(toLowerAscii(f8)).second) out.push_back(f8);
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return toLowerAscii(a) < toLowerAscii(b);
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
  DWORD exitCode = 0;
  uint32_t ms = 0;
  std::string spawnError;
};

HANDLE scanJob() {
  static HANDLE job = [] {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (j) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      // Helpers die with us; and a crashing helper must not show the WER dialog.
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
      SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return j;
  }();
  return job;
}

const wchar_t* scanDesktopName() {
  // Keep the desktop alive for the life of the process; children are started on it by name.
  static HDESK desk = CreateDesktopW(L"YeOldeVST-Scan", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  return desk ? L"YeOldeVST-Scan" : nullptr;
}

enum class Phase { All, NoEditor, EditorOnly };

ProbeOutcome runHelper(const std::string& exe, const std::string& path, int32_t shellId, const Scanner::Options& opt,
                       Phase phase, std::atomic<bool>* cancel, bool forceHidden = false) {
  ProbeOutcome o;
  std::wstring cmd = L"\"" + widen(exe) + L"\" --probe \"" + widen(path) + L"\"";
  if (shellId) cmd += L" --shell-id " + std::to_wstring(shellId);
  if (phase == Phase::All) cmd += L" --editor";
  if (phase == Phase::EditorOnly) cmd += L" --editor-only";

  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
    o.spawnError = "CreatePipe failed";
    return o;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  // Only the pipe's write end is inherited, even when several probes start at once.
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<uint8_t> attrBuf(attrSize);
  auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
  HANDLE inherit[1] = {wr};
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  // No STARTF_USESHOWWINDOW: it would turn the probe's first ShowWindow into a hide, and some
  // editors hang with an invisible parent. CREATE_NO_WINDOW already suppresses the console.
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = wr;
  si.StartupInfo.hStdError = wr;
  si.StartupInfo.hStdInput = nullptr;
  std::wstring desktop;
  // Editors misbehave on a desktop nobody can see, so the editor phase runs on the user's one
  // (its window is placed off-screen).
  if (opt.hiddenDesktop && (phase != Phase::EditorOnly || forceHidden)) {
    if (auto d = scanDesktopName()) {
      desktop = d;
      si.StartupInfo.lpDesktop = desktop.data();
    }
  }
  PROCESS_INFORMATION pi{};
  std::wstring dir = widen(dirOf(path));
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                           dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(wr);
  if (!ok) {
    o.spawnError = "could not start helper (error " + std::to_string(GetLastError()) + ")";
    CloseHandle(rd);
    return o;
  }
  if (HANDLE job = scanJob()) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  o.started = true;

  std::string buf;
  auto drain = [&] {
    DWORD avail = 0;
    while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      char tmp[4096];
      DWORD got = 0;
      if (!ReadFile(rd, tmp, std::min<DWORD>(avail, sizeof tmp), &got, nullptr) || got == 0) break;
      buf.append(tmp, got);
      if (buf.size() > (8u << 20)) buf.erase(0, buf.size() - (1u << 20));  // runaway output
    }
  };
  auto t0 = GetTickCount64();
  for (;;) {
    DWORD w = WaitForSingleObject(pi.hProcess, 25);
    drain();
    if (w == WAIT_OBJECT_0) break;
    if (cancel && cancel->load()) {
      TerminateProcess(pi.hProcess, 0xC000013A);
      WaitForSingleObject(pi.hProcess, 2000);
      o.cancelled = true;
      break;
    }
    if (GetTickCount64() - t0 > (uint64_t)opt.timeoutMs) {
      TerminateProcess(pi.hProcess, 0x102 /*WAIT_TIMEOUT*/);
      WaitForSingleObject(pi.hProcess, 2000);
      o.timedOut = true;
      break;
    }
  }
  drain();
  GetExitCodeProcess(pi.hProcess, &o.exitCode);
  o.ms = (uint32_t)(GetTickCount64() - t0);
  CloseHandle(pi.hProcess);
  CloseHandle(rd);

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
    std::string why = faultText.empty() ? (std::string(exceptionName(o.exitCode)) + " (exit code 0x" + [&] {
      char b[16];
      snprintf(b, sizeof b, "%08lX", o.exitCode);
      return std::string(b);
    }() + ")")
                                        : faultText;
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
  if (!opt.testEditors || !opt.hiddenDesktop || !r.hasEditor || !statusLoadable(r.status)) return;
  // A plugin that popped something up while loading would do it again: keep that hidden too.
  bool popups = false;
  for (auto& w : r.warnings)
    if (w.rfind("showed a popup", 0) == 0) popups = true;
  auto o = runHelper(exe, r.path, r.shellId, opt, Phase::EditorOnly, cancel, popups);
  if (o.cancelled) return;
  r.scanMs += o.ms;
  PluginRecord e = r;
  classify(e, o, nullptr);
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

std::vector<PluginRecord> Scanner::probeFile(const std::string& path, const Options& opt, std::atomic<bool>* cancel) {
  std::vector<PluginRecord> out;
  PluginRecord r;
  r.path = path;
  fileStamp(path, r.stamp);
  r.scannedAt = (uint64_t)time(nullptr);
  PeInfo pe = readPeInfo(path);
  r.arch = archName(pe.arch);
  if (!pe.valid || !pe.isDll) {
    r.status = ScanStatus::NotPlugin;
    r.detail = pe.error.empty() ? "not a DLL" : pe.error;
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
  if (pe.arch != PeArch::X64 && pe.arch != PeArch::X86) {
    r.status = ScanStatus::Unsupported;
    r.detail = std::string(archName(pe.arch)) + " plugins are not supported";
    out.push_back(r);
    return out;
  }
  std::string exe = helperExePath(pe.arch);
  if (exe.empty()) {
    r.status = ScanStatus::NoHelper;
    r.detail = pe.arch == PeArch::X86 ? "32-bit plugin, but the 32-bit helper (YeOldeVST-Host32.exe) is missing"
                                      : "the 64-bit helper (YeOldeVST-Host64.exe) is missing";
    out.push_back(r);
    return out;
  }

  const Phase first = !opt.testEditors ? Phase::NoEditor : opt.hiddenDesktop ? Phase::NoEditor : Phase::All;
  std::vector<std::pair<int32_t, std::string>> kids;
  auto o = runHelper(exe, path, 0, opt, first, cancel);
  if (o.cancelled) return {};
  classify(r, o, &kids);
  if (r.name.empty()) r.name = r.product;
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
  HANDLE sem = CreateSemaphoreW(nullptr, 1, 1, L"Local\\YeOldeVST-ScanLock");
  if (sem && WaitForSingleObject(sem, 0) != WAIT_OBJECT_0) {
    CloseHandle(sem);
    std::lock_guard<std::mutex> lk(m_);
    prog_.running = false;
    prog_.message = "Another YeOldeVST instance is scanning right now. Try again when it is done.";
    running_ = false;
    return;
  }

  db_.reloadIfChanged();
  std::vector<std::string> files = opt.onlyFiles.empty() ? findPluginFiles(opt.folders) : opt.onlyFiles;

  // Decide what needs probing.
  std::vector<std::string> work;
  bool host32 = !helperExePath(PeArch::X86).empty();
  for (auto& f : files) {
    PluginRecord old;
    FileStamp st;
    fileStamp(f, st);
    bool have = db_.find(f, 0, old);
    bool needs = !have || opt.rescanAll || !opt.onlyFiles.empty() || !(old.stamp == st) ||
                 old.status == ScanStatus::Unknown || (old.status == ScanStatus::NoHelper && host32) ||
                 (opt.retryFailed && statusAngry(old.status));
    if (needs) work.push_back(f);
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    prog_.total = (int)work.size();
    prog_.message = work.empty() ? "Everything is up to date." : "Scanning...";
  }
  logLine("scan: %d files found, %d to probe", (int)files.size(), (int)work.size());

  std::atomic<size_t> next{0};
  std::mutex saveM;
  uint64_t lastSave = GetTickCount64();
  auto worker = [&] {
    for (;;) {
      if (cancel_.load()) return;
      size_t i = next.fetch_add(1);
      if (i >= work.size()) return;
      const std::string& f = work[i];
      {
        std::lock_guard<std::mutex> lk(m_);
        prog_.current.push_back(f);
      }
      auto recs = probeFile(f, opt, &cancel_);
      if (!recs.empty()) {
        db_.removeShellChildren(f);
        for (auto& r : recs) {
          db_.upsert(r);
          if (onResult) onResult(r);
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
      if (GetTickCount64() - lastSave > 3000) {
        db_.save();  // keep progress if the DAW dies mid-scan
        lastSave = GetTickCount64();
      }
    }
  };
  int n = std::clamp(opt.parallel, 1, 16);
  std::vector<std::thread> pool;
  for (int i = 0; i < n; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  if (opt.onlyFiles.empty() && !cancel_.load()) db_.prune(opt.folders);
  db_.save();
  if (sem) {
    ReleaseSemaphore(sem, 1, nullptr);
    CloseHandle(sem);
  }
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
