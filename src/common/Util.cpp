// Portable helpers. The parts that talk to the OS live in platform/win/UtilWin.cpp and
// platform/posix/UtilPosix.cpp.
#include "common/Util.h"

namespace yov {

std::string toLowerAscii(std::string_view s) {
  std::string r(s);
  for (auto& c : r)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return r;
}

std::string pathKey(std::string_view path) {
#if defined(_WIN32) || defined(__APPLE__)
  return foldCase(path);
#else
  return std::string(path);
#endif
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

std::string fileNameOf(std::string_view path) {
  auto p = path.find_last_of("\\/");
  return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}

std::string dirOf(std::string_view path) {
  auto p = path.find_last_of("\\/");
  if (p == std::string_view::npos) return std::string();
#ifndef _WIN32
  if (p == 0) return std::string(path.substr(0, 1));  // "/file" -> "/"
#endif
  return std::string(path.substr(0, p));
}

std::string joinPath(std::string_view a, std::string_view b) {
  if (a.empty()) return std::string(b);
  std::string r(a);
  if (r.back() != '\\' && r.back() != '/') r += kPathSep;
  r += b;
  return r;
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

}  // namespace yov
