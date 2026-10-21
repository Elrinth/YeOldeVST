#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace yov {

#ifdef _WIN32
constexpr char kPathSep = '\\';
#else
constexpr char kPathSep = '/';
#endif

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Lower-case ASCII copy (used for case-insensitive path / extension comparisons).
std::string toLowerAscii(std::string_view s);
// Unicode-aware lower-casing (Cyrillic, accented letters...): for comparing paths and searching.
std::string foldCase(std::string_view s);
// What two paths are compared by: case-folded where the file system ignores case (Windows,
// macOS), the path itself where it does not (Linux).
std::string pathKey(std::string_view path);
// A path an old ANSI-only plugin can use: the path itself if the system code page can hold it,
// otherwise its 8.3 short form (plain ASCII) when the volume has short names. Unchanged on
// systems whose plugins take UTF-8.
std::string ansiPath(const std::string& utf8Path);
// A string an old plugin wrote into a char buffer, as UTF-8: plugins of that era used the ANSI
// code page (Windows) or Latin-1, not UTF-8. Stops at the terminator; trims.
std::string fromPluginString(const char* buf, size_t cap);
bool endsWithNoCase(std::string_view s, std::string_view suffix);
bool containsNoCase(std::string_view haystack, std::string_view needle);
std::string trim(std::string_view s);

// Per-user settings folder: %APPDATA%\YeOldeVST, ~/.config/YeOldeVST, ~/Library/Application
// Support/YeOldeVST (created on demand). UTF-8. YEOLDEVST_DATA_DIR overrides it (tests).
std::string appDataDir();
// Per-user folder for helpers and caches: %LOCALAPPDATA%\YeOldeVST, ~/.local/share/YeOldeVST,
// ~/Library/Caches/YeOldeVST (created on demand). UTF-8.
std::string localAppDataDir();
// Directory holding the module (DLL/EXE/.so) that contains this code. UTF-8, no trailing slash.
std::string thisModuleDir();
std::string thisModulePath();

std::string fileNameOf(std::string_view path);
std::string dirOf(std::string_view path);
std::string joinPath(std::string_view a, std::string_view b);
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
bool makeDirs(const std::string& path);
bool removeFile(const std::string& path);
bool removeDir(const std::string& path);  // empty directories only
// Marks a file executable (POSIX). No-op on Windows.
bool makeExecutable(const std::string& path);

struct DirEntry {
  std::string name;  // UTF-8, no directory part
  bool isDir = false;
  bool isLink = false;  // symlink / junction: not followed by the scanner (loops)
};
// Lists a directory (without "." and ".."). False if it cannot be read.
bool listDir(const std::string& dir, std::vector<DirEntry>& out);

std::string currentDir();
bool setCurrentDir(const std::string& dir);

struct FileStamp {
  uint64_t size = 0;
  uint64_t mtime = 0;  // FILETIME as 64-bit (Windows), nanoseconds since 1970 elsewhere
  bool operator==(const FileStamp&) const = default;
};
bool fileStamp(const std::string& path, FileStamp& out);

bool readFile(const std::string& path, std::string& out);
// Reads at most maxBytes from the start of the file.
bool readFileHead(const std::string& path, size_t maxBytes, std::string& out);
// Writes to a temp file then renames, so readers never see a half-written file.
// executable: POSIX mode 0755 before it appears under its name (Windows: ignored).
bool writeFileAtomic(const std::string& path, std::string_view data, bool executable = false);
// fopen with a UTF-8 path.
FILE* openFile(const std::string& path, const char* mode);

std::string base64Encode(const void* data, size_t size);
bool base64Decode(std::string_view in, std::vector<uint8_t>& out);

// Append a line to <appDataDir>/YeOldeVST.log (thread-safe, best effort).
void logLine(const char* fmt, ...);

// Milliseconds from a monotonic clock.
uint64_t nowMs();
uint64_t currentThreadId();
uint32_t currentProcessId();
// Reads an environment variable as UTF-8. False if it is not set.
bool getEnv(const char* name, std::string& out);

}  // namespace yov
