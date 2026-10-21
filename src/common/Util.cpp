#include "common/Util.h"

#include <windows.h>
#include <shlobj.h>

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

std::string toLowerAscii(std::string_view s) {
  std::string r(s);
  for (auto& c : r)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return r;
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

bool endsWithNoCase(std::string_view s, std::string_view suffix) {
  if (suffix.size() > s.size()) return false;
  return toLowerAscii(s.substr(s.size() - suffix.size())) == toLowerAscii(suffix);
}

bool containsNoCase(std::string_view haystack, std::string_view needle) {
  return toLowerAscii(haystack).find(toLowerAscii(needle)) != std::string::npos;
}

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (unsigned char)s[b] <= ' ') ++b;
  while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
  return std::string(s.substr(b, e - b));
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

std::string fileNameOf(std::string_view path) {
  auto p = path.find_last_of("\\/");
  return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}

std::string dirOf(std::string_view path) {
  auto p = path.find_last_of("\\/");
  return p == std::string_view::npos ? std::string() : std::string(path.substr(0, p));
}

std::string joinPath(std::string_view a, std::string_view b) {
  if (a.empty()) return std::string(b);
  std::string r(a);
  if (r.back() != '\\' && r.back() != '/') r += '\\';
  r += b;
  return r;
}

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

bool writeFileAtomic(const std::string& path, std::string_view data) {
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

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const void* data, size_t size) {
  auto p = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i < size) {
    uint32_t v = p[i] << 16;
    if (i + 1 < size) v |= p[i + 1] << 8;
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += (i + 1 < size) ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

bool base64Decode(std::string_view in, std::vector<uint8_t>& out) {
  int8_t map[256];
  for (auto& m : map) m = -1;
  for (int i = 0; i < 64; ++i) map[(uint8_t)kB64[i]] = (int8_t)i;
  out.clear();
  out.reserve(in.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
    int v = map[(uint8_t)c];
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((acc >> bits) & 0xFF));
    }
  }
  return true;
}

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

}  // namespace yov
