// Linux / macOS side of common/Util.h.
#include "common/Util.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace yov {

namespace {

// Decodes one UTF-8 sequence at s[i]; returns the code point and advances i. -1 if invalid.
int32_t decodeUtf8(std::string_view s, size_t& i) {
  unsigned char c = (unsigned char)s[i];
  int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (!len || i + len > s.size()) return -1;
  int32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
  for (int k = 1; k < len; ++k) {
    unsigned char cc = (unsigned char)s[i + k];
    if ((cc & 0xC0) != 0x80) return -1;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF))
    return -1;
  i += len;
  return cp;
}

void encodeUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

bool validUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();)
    if (decodeUtf8(s, i) < 0) return false;
  return true;
}

// Lower-casing for the scripts plugin and folder names realistically use.
uint32_t lowerCodePoint(uint32_t c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;                         // Latin-1
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c | 1;     // Latin Ext-A pairs
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
  if (c == 0x178) return 0xFF;
  if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;                      // Greek
  if (c >= 0x410 && c <= 0x42F) return c + 32;                                     // Cyrillic
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  if (c >= 0x460 && c <= 0x4FF && !(c & 1)) return c + 1;
  return c;
}

std::string homeDir() {
  if (const char* h = getenv("HOME"); h && *h) return h;
  if (passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
  return "/tmp";
}

std::string xdgDir(const char* var, const char* fallback) {
  if (const char* v = getenv(var); v && *v == '/') return v;
  return joinPath(homeDir(), fallback);
}

}  // namespace

std::wstring widen(std::string_view utf8) {
  std::wstring w;
  w.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    size_t at = i;
    int32_t cp = decodeUtf8(utf8, i);
    if (cp < 0) {
      cp = (unsigned char)utf8[at];  // not UTF-8: treat the byte as Latin-1
      i = at + 1;
    }
    w += (wchar_t)cp;
  }
  return w;
}

std::string narrow(std::wstring_view wide) {
  std::string s;
  s.reserve(wide.size());
  for (wchar_t c : wide) encodeUtf8((uint32_t)c, s);
  return s;
}

std::string foldCase(std::string_view s) {
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) ascii = false;
  if (ascii) return toLowerAscii(s);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t at = i;
    int32_t cp = decodeUtf8(s, i);
    if (cp < 0) {
      out += s[at];
      i = at + 1;
      continue;
    }
    encodeUtf8(lowerCodePoint((uint32_t)cp), out);
  }
  return out;
}

std::string ansiPath(const std::string& utf8Path) { return utf8Path; }

std::string fromPluginString(const char* buf, size_t cap) {
  size_t n = strnlen(buf, cap);
  std::string s(buf, n);
  if (!validUtf8(s)) {
    // Old plugins wrote Latin-1 / Windows-1252.
    std::string u;
    for (unsigned char c : s) encodeUtf8(c, u);
    s = u;
  }
  return trim(s);
}

std::string appDataDir() {
  std::string d;
  if (!getEnv("YEOLDEVST_DATA_DIR", d) || d.empty()) {
#ifdef __APPLE__
    d = joinPath(homeDir(), "Library/Application Support/YeOldeVST");
#else
    d = joinPath(xdgDir("XDG_CONFIG_HOME", ".config"), "YeOldeVST");
#endif
  }
  makeDirs(d);
  return d;
}

std::string localAppDataDir() {
#ifdef __APPLE__
  auto d = joinPath(homeDir(), "Library/Application Support/YeOldeVST");
#else
  auto d = joinPath(xdgDir("XDG_DATA_HOME", ".local/share"), "YeOldeVST");
#endif
  makeDirs(d);
  return d;
}

std::string thisModulePath() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&thisModulePath), &info) || !info.dli_fname) return {};
  char real[PATH_MAX];
  if (realpath(info.dli_fname, real)) return real;
  return info.dli_fname;
}

std::string thisModuleDir() { return dirOf(thisModulePath()); }

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

bool dirExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool makeDirs(const std::string& path) {
  if (path.empty() || dirExists(path)) return true;
  auto parent = dirOf(path);
  if (!parent.empty() && parent != path) makeDirs(parent);
  return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool removeFile(const std::string& path) { return unlink(path.c_str()) == 0; }
bool removeDir(const std::string& path) { return rmdir(path.c_str()) == 0; }

bool makeExecutable(const std::string& path) { return chmod(path.c_str(), 0755) == 0; }

bool listDir(const std::string& dir, std::vector<DirEntry>& out) {
  out.clear();
  DIR* d = opendir(dir.c_str());
  if (!d) return false;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    DirEntry de;
    de.name = name;
    struct stat lst;
    std::string full = joinPath(dir, name);
    if (lstat(full.c_str(), &lst) != 0) continue;
    de.isLink = S_ISLNK(lst.st_mode);
    if (de.isLink) {
      // A link to a file is fine to use; links to folders are not followed (loops).
      struct stat st;
      if (stat(full.c_str(), &st) != 0) continue;  // dangling
      de.isDir = S_ISDIR(st.st_mode);
      if (!de.isDir) de.isLink = false;
    } else {
      de.isDir = S_ISDIR(lst.st_mode);
    }
    out.push_back(std::move(de));
  }
  closedir(d);
  return true;
}

std::string currentDir() {
  char buf[PATH_MAX];
  return getcwd(buf, sizeof buf) ? std::string(buf) : std::string();
}

bool setCurrentDir(const std::string& dir) { return chdir(dir.c_str()) == 0; }

bool fileStamp(const std::string& path, FileStamp& out) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  out.size = (uint64_t)st.st_size;
#ifdef __APPLE__
  out.mtime = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec;
#else
  out.mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
  return true;
}

bool readFileHead(const std::string& path, size_t maxBytes, std::string& out) {
  out.clear();
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buf[65536];
  while (out.size() < maxBytes) {
    ssize_t n = read(fd, buf, std::min(sizeof buf, maxBytes - out.size()));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  close(fd);
  return true;
}

bool readFile(const std::string& path, std::string& out) { return readFileHead(path, SIZE_MAX, out); }

bool writeFileAtomic(const std::string& path, std::string_view data) {
  auto tmp = path + ".tmp" + std::to_string(getpid());
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  size_t done = 0;
  bool ok = true;
  while (done < data.size()) {
    ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    done += (size_t)n;
  }
  if (ok) fsync(fd);
  close(fd);
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

FILE* openFile(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }

void logLine(const char* fmt, ...) {
  static std::mutex m;
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm lt{};
  localtime_r(&ts.tv_sec, &lt);
  char line[2200];
  int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%d] %s\n", lt.tm_year + 1900,
                   lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (int)(ts.tv_nsec / 1000000),
                   (int)getpid(), buf);
  if (n < 0) return;
  n = std::min<int>(n, (int)sizeof(line) - 1);
  std::lock_guard<std::mutex> lk(m);
  static std::string path = joinPath(appDataDir(), "YeOldeVST.log");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return;
  ssize_t w = write(fd, line, (size_t)n);
  (void)w;
  close(fd);
}

uint64_t nowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

uint64_t currentThreadId() {
#if defined(__linux__)
  return (uint64_t)syscall(SYS_gettid);
#else
  uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return id;
#endif
}

uint32_t currentProcessId() { return (uint32_t)getpid(); }

bool getEnv(const char* name, std::string& out) {
  const char* v = getenv(name);
  if (!v) return false;
  out = v;
  return true;
}

}  // namespace yov
