#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yov {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Lower-case ASCII copy (used for case-insensitive path / extension comparisons).
std::string toLowerAscii(std::string_view s);
// Unicode-aware lower-casing (Cyrillic, accented letters...): for comparing paths and searching.
std::string foldCase(std::string_view s);
// A path an old ANSI-only plugin can use: the path itself if the system code page can hold it,
// otherwise its 8.3 short form (plain ASCII) when the volume has short names.
std::string ansiPath(const std::string& utf8Path);
bool endsWithNoCase(std::string_view s, std::string_view suffix);
bool containsNoCase(std::string_view haystack, std::string_view needle);
std::string trim(std::string_view s);

// %APPDATA%\YeOldeVST (created on demand). UTF-8. YEOLDEVST_DATA_DIR overrides it (tests).
std::string appDataDir();
// %LOCALAPPDATA%\YeOldeVST (created on demand). UTF-8.
std::string localAppDataDir();
// Directory holding the module (DLL/EXE) that contains this code. UTF-8, no trailing slash.
std::string thisModuleDir();
std::string thisModulePath();

std::string fileNameOf(std::string_view path);
std::string dirOf(std::string_view path);
std::string joinPath(std::string_view a, std::string_view b);
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
bool makeDirs(const std::string& path);

struct FileStamp {
  uint64_t size = 0;
  uint64_t mtime = 0;  // FILETIME as 64-bit
  bool operator==(const FileStamp&) const = default;
};
bool fileStamp(const std::string& path, FileStamp& out);

bool readFile(const std::string& path, std::string& out);
// Writes to a temp file then renames, so readers never see a half-written file.
bool writeFileAtomic(const std::string& path, std::string_view data);

std::string base64Encode(const void* data, size_t size);
bool base64Decode(std::string_view in, std::vector<uint8_t>& out);

// Append a line to %APPDATA%\YeOldeVST\YeOldeVST.log (thread-safe, best effort).
void logLine(const char* fmt, ...);

uint64_t nowMs();

}  // namespace yov
