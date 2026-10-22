#include "platform/Process.h"

#include "common/Seh.h"
#include "common/Util.h"

#include <windows.h>

#include <algorithm>

namespace yov {

namespace {

HANDLE jobFor(SpawnOptions::Group g) {
  auto make = [] {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (j) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      // Helpers die with us; and a crashing helper must not show the WER dialog.
      li.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
      SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return j;
  };
  static HANDLE scan = make();
  static HANDLE bridge = make();
  return g == SpawnOptions::Group::Scan ? scan : g == SpawnOptions::Group::Bridge ? bridge : nullptr;
}

const wchar_t* scanDesktopName() {
  // Keep the desktop alive for the life of the process; children are started on it by name.
  static HDESK desk = CreateDesktopW(L"YeOldeVST-Scan", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  return desk ? L"YeOldeVST-Scan" : nullptr;
}

// Quotes one argument the way CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring q = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
    } else if (c == L'"') {
      q.append(slashes + 1, L'\\');  // double the backslashes before a quote, then escape it
      slashes = 0;
    } else {
      slashes = 0;
    }
    q += c;
  }
  q.append(slashes, L'\\');
  q += L'"';
  return q;
}

}  // namespace

struct ChildProcess::Impl {
  HANDLE proc = nullptr;
  HANDLE out = nullptr;
  DWORD pid = 0;
  DWORD code = 0;
};

ChildProcess::ChildProcess() : d_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
  if (d_->proc) CloseHandle(d_->proc);
  if (d_->out) CloseHandle(d_->out);
}

bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args, const SpawnOptions& opt,
                         std::string& error) {
  std::wstring cmd = L"\"" + widen(exe) + L"\"";
  for (auto& a : args) cmd += L" " + quoteArg(widen(a));

  std::wstring dir = widen(opt.cwd);
  PROCESS_INFORMATION pi{};
  if (opt.captureOutput) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
      error = "CreatePipe failed";
      return false;
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
    si.lpAttributeList = attrs;
    std::wstring desktop;
    if (opt.hiddenDesktop) {
      if (auto d = scanDesktopName()) {
        desktop = d;
        si.StartupInfo.lpDesktop = desktop.data();
      }
    }
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                             dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (!ok) {
      error = "could not start helper (error " + std::to_string(GetLastError()) + ")";
      CloseHandle(rd);
      return false;
    }
    d_->out = rd;
  } else {
    STARTUPINFOW si{sizeof si};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
      error = "could not start the helper process (error " + std::to_string(GetLastError()) + ")";
      return false;
    }
  }
  if (HANDLE job = jobFor(opt.group)) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  d_->proc = pi.hProcess;
  d_->pid = pi.dwProcessId;
  if (opt.allowForeground) AllowSetForegroundWindow(d_->pid);
  return true;
}

bool ChildProcess::started() const { return d_->proc != nullptr; }
uint32_t ChildProcess::pid() const { return d_->pid; }
void* ChildProcess::nativeHandle() const { return d_->proc; }

void ChildProcess::readOutput(std::string& buf) {
  if (!d_->out) return;
  DWORD avail = 0;
  while (PeekNamedPipe(d_->out, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
    char tmp[4096];
    DWORD got = 0;
    if (!ReadFile(d_->out, tmp, std::min<DWORD>(avail, sizeof tmp), &got, nullptr) || got == 0) break;
    buf.append(tmp, got);
    if (buf.size() > (8u << 20)) buf.erase(0, buf.size() - (1u << 20));  // runaway output
  }
}

bool ChildProcess::wait(uint32_t ms) {
  if (!d_->proc) return true;
  if (WaitForSingleObject(d_->proc, ms) != WAIT_OBJECT_0) return false;
  GetExitCodeProcess(d_->proc, &d_->code);
  return true;
}

void ChildProcess::kill(uint32_t code) {
  if (d_->proc) TerminateProcess(d_->proc, code);
}

uint32_t ChildProcess::exitCode() const { return d_->code; }
bool ChildProcess::signaled() const { return false; }

std::string ChildProcess::exitDescription(const char* codeLabel) const {
  char b[160];
  snprintf(b, sizeof b, "%s (%s0x%08lX)", exceptionName(d_->code), codeLabel, d_->code);
  return b;
}

// ---- ProcessWatch ----

ProcessWatch::ProcessWatch() = default;
ProcessWatch::~ProcessWatch() {
  if (h_) CloseHandle(h_);
}
bool ProcessWatch::open(uint32_t pid) {
  pid_ = pid;
  h_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
  return h_ != nullptr;
}
bool ProcessWatch::gone() { return h_ && WaitForSingleObject(h_, 0) == WAIT_OBJECT_0; }
void* ProcessWatch::nativeHandle() const { return h_; }

bool haveDesktop() { return true; }
void hiddenDesktopAcquire() {}
void hiddenDesktopRelease() {}

// ---- SystemLock ----

SystemLock::~SystemLock() { release(); }

bool SystemLock::tryAcquire(const char* name) {
  HANDLE sem = CreateSemaphoreW(nullptr, 1, 1, (L"Local\\" + widen(name)).c_str());
  if (sem && WaitForSingleObject(sem, 0) != WAIT_OBJECT_0) {
    CloseHandle(sem);
    return false;
  }
  h_ = sem;  // null if the semaphore could not be created: scan anyway
  return true;
}

void SystemLock::release() {
  if (h_) {
    ReleaseSemaphore(h_, 1, nullptr);
    CloseHandle(h_);
    h_ = nullptr;
  }
}

}  // namespace yov
