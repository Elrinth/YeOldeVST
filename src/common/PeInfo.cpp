#include "common/PeInfo.h"

#include "common/Util.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace yov {

const char* archName(PeArch a) {
  switch (a) {
    case PeArch::X86: return "x86";
    case PeArch::X64: return "x64";
    case PeArch::Arm64: return "arm64";
    case PeArch::Other: return "other";
    default: return "unknown";
  }
}

namespace {

struct Section {
  uint32_t va, vsize, rawPtr, rawSize;
};

struct Reader {
  std::string data;
  template <class T>
  bool read(size_t off, T& out) const {
    if (off + sizeof(T) > data.size() || off + sizeof(T) < off) return false;
    std::memcpy(&out, data.data() + off, sizeof(T));
    return true;
  }
};

bool rvaToOffset(const std::vector<Section>& secs, uint32_t rva, size_t& off) {
  for (auto& s : secs) {
    uint32_t size = s.vsize ? s.vsize : s.rawSize;
    if (rva >= s.va && rva < s.va + size) {
      off = size_t(rva - s.va) + s.rawPtr;
      return true;
    }
  }
  return false;
}

}  // namespace

PeInfo readPeInfo(const std::string& path) {
  PeInfo info;
  Reader r;
  // Plugin DLLs can be large (sample data in resources); the headers and export table are what
  // we need. Read up to 64 MB, which covers the export table of anything realistic.
  {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      info.error = "cannot open file";
      return info;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    size_t want = (size_t)std::min<long long>(sz.QuadPart, 64ll << 20);
    r.data.resize(want);
    DWORD got = 0;
    ReadFile(h, r.data.data(), (DWORD)want, &got, nullptr);
    r.data.resize(got);
    CloseHandle(h);
  }

  IMAGE_DOS_HEADER dos{};
  if (!r.read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
    info.error = "not a PE file";
    return info;
  }
  uint32_t sig = 0;
  if (!r.read(dos.e_lfanew, sig) || sig != IMAGE_NT_SIGNATURE) {
    info.error = "not a PE file";
    return info;
  }
  IMAGE_FILE_HEADER fh{};
  size_t fhOff = size_t(dos.e_lfanew) + 4;
  if (!r.read(fhOff, fh)) {
    info.error = "truncated header";
    return info;
  }
  switch (fh.Machine) {
    case IMAGE_FILE_MACHINE_I386: info.arch = PeArch::X86; break;
    case IMAGE_FILE_MACHINE_AMD64: info.arch = PeArch::X64; break;
    case IMAGE_FILE_MACHINE_ARM64: info.arch = PeArch::Arm64; break;
    default: info.arch = PeArch::Other; break;
  }
  info.isDll = (fh.Characteristics & IMAGE_FILE_DLL) != 0;

  size_t optOff = fhOff + sizeof(IMAGE_FILE_HEADER);
  uint16_t magic = 0;
  if (!r.read(optOff, magic)) {
    info.error = "truncated optional header";
    return info;
  }
  IMAGE_DATA_DIRECTORY exportDir{};
  if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    IMAGE_OPTIONAL_HEADER32 oh{};
    if (!r.read(optOff, oh)) return info;
    if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) exportDir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    IMAGE_OPTIONAL_HEADER64 oh{};
    if (!r.read(optOff, oh)) return info;
    if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) exportDir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  } else {
    info.error = "unknown optional header";
    return info;
  }

  std::vector<Section> secs;
  size_t secOff = optOff + fh.SizeOfOptionalHeader;
  for (int i = 0; i < fh.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER sh{};
    if (!r.read(secOff + size_t(i) * sizeof(sh), sh)) break;
    secs.push_back({sh.VirtualAddress, sh.Misc.VirtualSize, sh.PointerToRawData, sh.SizeOfRawData});
  }
  info.valid = true;

  if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) return info;
  size_t expOff;
  if (!rvaToOffset(secs, exportDir.VirtualAddress, expOff)) return info;
  IMAGE_EXPORT_DIRECTORY ed{};
  if (!r.read(expOff, ed)) return info;
  size_t namesOff;
  if (!rvaToOffset(secs, ed.AddressOfNames, namesOff)) return info;
  uint32_t count = std::min<uint32_t>(ed.NumberOfNames, 100000);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t nameRva = 0;
    if (!r.read(namesOff + size_t(i) * 4, nameRva)) break;
    size_t nOff;
    if (!rvaToOffset(secs, nameRva, nOff) || nOff >= r.data.size()) continue;
    const char* s = r.data.data() + nOff;
    size_t maxLen = r.data.size() - nOff;
    size_t len = strnlen(s, std::min<size_t>(maxLen, 256));
    std::string_view name(s, len);
    if (name == "VSTPluginMain") info.hasVstPluginMain = true;
    else if (name == "main" || name == "main_plugin" || name == "_main" || name == "MAIN") info.hasMain = true;
    else if (name == "GetPluginFactory") info.hasVst3Factory = true;
    else if (name == "clap_entry") info.hasClapEntry = true;
  }
  return info;
}

}  // namespace yov
