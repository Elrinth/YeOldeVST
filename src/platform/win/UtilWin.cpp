// Windows side of common/Util.h.
#include "common/Util.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace yov {

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  if (n <= 0) {
    // Not valid UTF-8 (plugins return ANSI strings): fall back to the ANSI code page.
    n = MultiByteToWideChar(CP_ACP, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
  }
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
  return w;
}

std::string narrow(std::wstring_view wide) {
  if (wide.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::string foldCase(std::string_view s) {
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) ascii = false;
  if (ascii) return toLowerAscii(s);
  std::wstring w = widen(s);
  if (w.empty()) return {};
  std::wstring out(w.size() * 2 + 8, L'\0');
  int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), (int)w.size(), out.data(), (int)out.size(),
                        nullptr, nullptr, 0);
  if (n <= 0) return toLowerAscii(s);
  out.resize(n);
  return narrow(out);
}

std::string ansiPath(const std::string& utf8Path) {
  std::wstring w = widen(utf8Path);
  BOOL lossy = FALSE;
  int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), nullptr, 0, nullptr, &lossy);
  if (!lossy) {
    std::string a(n, '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), a.data(), n, nullptr, nullptr);
    return a;
  }
  wchar_t shortBuf[MAX_PATH * 2];
  DWORD sn = GetShortPathNameW(w.c_str(), shortBuf, MAX_PATH * 2);
  std::wstring use = (sn > 0 && sn < MAX_PATH * 2) ? std::wstring(shortBuf, sn) : w;
  n = WideCharToMultiByte(CP_ACP, 0, use.c_str(), (int)use.size(), nullptr, 0, nullptr, nullptr);
  std::string a(n, '\0');
  WideCharToMultiByte(CP_ACP, 0, use.c_str(), (int)use.size(), a.data(), n, nullptr, nullptr);
  return a;
}

std::string fromPluginString(const char* buf, size_t cap) {
  size_t n = strnlen(buf, cap);
  std::string s(buf, n);
  // Old plugins return ANSI (Windows-1252 etc.); convert so the UI shows them right.
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) ascii = false;
  if (!ascii) {
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (wn <= 0) {
      wn = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
      std::wstring w(wn, L'\0');
      MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), wn);
      s = narrow(w);
    }
  }
  return trim(s);
}

static std::string knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::string r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) r = narrow(p);
  CoTaskMemFree(p);
  return r;
}

std::string appDataDir() {
  wchar_t over[1024];
  DWORD on = GetEnvironmentVariableW(L"YEOLDEVST_DATA_DIR", over, 1024);
  if (on > 0 && on < 1024) {
    auto d = narrow(std::wstring(over, on));
    makeDirs(d);
    return d;
  }
  auto d = joinPath(knownFolder(FOLDERID_RoamingAppData), "YeOldeVST");
  makeDirs(d);
  return d;
}

std::string localAppDataDir() {
  auto d = joinPath(knownFolder(FOLDERID_LocalAppData), "YeOldeVST");
  makeDirs(d);
  return d;
}

std::string thisModulePath() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&thisModulePath), &mod);
  std::wstring buf(32768, L'\0');
  DWORD n = GetModuleFileNameW(mod, buf.data(), (DWORD)buf.size());
  buf.resize(n);
  return narrow(buf);
}

std::string thisModuleDir() { return dirOf(thisModulePath()); }

bool fileExists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirs(const std::string& path) {
  if (path.empty() || dirExists(path)) return true;
  auto parent = dirOf(path);
  if (!parent.empty() && parent != path && !(parent.size() == 2 && parent[1] == ':')) makeDirs(parent);
  return CreateDirectoryW(widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeFile(const std::string& path) { return DeleteFileW(widen(path).c_str()) != 0; }
bool removeDir(const std::string& path) { return RemoveDirectoryW(widen(path).c_str()) != 0; }
bool makeExecutable(const std::string&) { return true; }

bool listDir(const std::string& dir, std::vector<DirEntry>& out) {
  out.clear();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((widen(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                              FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    DirEntry e;
    e.name = narrow(name);
    e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.isLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out.push_back(std::move(e));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
}

std::string currentDir() {
  wchar_t buf[4096];
  DWORD n = GetCurrentDirectoryW(4096, buf);
  if (n > 0 && n < 4096) return narrow(std::wstring(buf, n));
  return {};
}

bool setCurrentDir(const std::string& dir) { return SetCurrentDirectoryW(widen(dir).c_str()) != 0; }

bool fileStamp(const std::string& path, FileStamp& out) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return false;
  out.size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  out.mtime = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
  return true;
}

bool readFile(const std::string& path, std::string& out) {
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  out.resize((size_t)sz.QuadPart);
  DWORD got = 0;
  bool ok = sz.QuadPart == 0 || ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
  CloseHandle(h);
  out.resize(got);
  return ok;
}

bool readFileHead(const std::string& path, size_t maxBytes, std::string& out) {
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  size_t want = (size_t)std::min<long long>(sz.QuadPart, (long long)maxBytes);
  out.resize(want);
  DWORD got = 0;
  ReadFile(h, out.data(), (DWORD)want, &got, nullptr);
  out.resize(got);
  CloseHandle(h);
  return true;
}

bool writeFileAtomic(const std::string& path, std::string_view data, bool) {
  auto tmp = path + ".tmp" + std::to_string(GetCurrentProcessId());
  HANDLE h = CreateFileW(widen(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
  CloseHandle(h);
  if (!ok) {
    DeleteFileW(widen(tmp).c_str());
    return false;
  }
  if (!MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(widen(tmp).c_str());
    return false;
  }
  return true;
}

FILE* openFile(const std::string& path, const char* mode) { return _wfopen(widen(path).c_str(), widen(mode).c_str()); }

void logLine(const char* fmt, ...) {
  static std::mutex m;
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2200];
  int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\r\n", st.wYear, st.wMonth,
                   st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), buf);
  std::lock_guard<std::mutex> lk(m);
  static std::wstring path = widen(joinPath(appDataDir(), "YeOldeVST.log"));
  HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD w;
  WriteFile(h, line, (DWORD)n, &w, nullptr);
  CloseHandle(h);
}

uint64_t nowMs() { return GetTickCount64(); }
uint64_t currentThreadId() { return GetCurrentThreadId(); }
uint32_t currentProcessId() { return GetCurrentProcessId(); }

bool getEnv(const char* name, std::string& out) {
  wchar_t buf[4096];
  DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buf, 4096);
  if (n == 0 || n >= 4096) return false;
  out = narrow(std::wstring(buf, n));
  return true;
}

}  // namespace yov
