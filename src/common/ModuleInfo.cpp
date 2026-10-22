#include "common/ModuleInfo.h"

#include "common/Util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace yov {

const char* archName(Arch a) {
  switch (a) {
    case Arch::X86: return "x86";
    case Arch::X64: return "x64";
    case Arch::Arm64: return "arm64";
    case Arch::Other: return "other";
    default: return "unknown";
  }
}

const char* formatName(BinaryFormat f) {
  switch (f) {
    case BinaryFormat::PE: return "Windows";
    case BinaryFormat::ELF: return "Linux";
    case BinaryFormat::MachO: return "macOS";
    default: return "unknown";
  }
}

BinaryFormat nativeFormat() {
#if defined(_WIN32)
  return BinaryFormat::PE;
#elif defined(__APPLE__)
  return BinaryFormat::MachO;
#else
  return BinaryFormat::ELF;
#endif
}

Arch nativeArch() {
#if defined(_M_X64) || defined(__x86_64__)
  return Arch::X64;
#elif defined(_M_IX86) || defined(__i386__)
  return Arch::X86;
#elif defined(_M_ARM64) || defined(__aarch64__)
  return Arch::Arm64;
#else
  return Arch::Other;
#endif
}

namespace {

// Random access to the file. `limit` caps how far reads may go (the PE reader looks at the first
// 64 MB only, which covers the export table of anything realistic).
struct Reader {
  FILE* f = nullptr;
  uint64_t size = 0;
  uint64_t limit = UINT64_MAX;
  bool bigEndian = false;

  ~Reader() {
    if (f) fclose(f);
  }
  bool open(const std::string& path) {
    f = openFile(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) == 0) {
#ifdef _WIN32
      size = (uint64_t)_ftelli64(f);
#else
      size = (uint64_t)ftello(f);
#endif
    }
    return true;
  }
  bool bytes(uint64_t off, void* out, size_t n) const {
    uint64_t end = std::min(size, limit);
    if (off + n > end || off + n < off) return false;
#ifdef _WIN32
    if (_fseeki64(f, (long long)off, SEEK_SET) != 0) return false;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return false;
#endif
    return fread(out, 1, n, f) == n;
  }
  template <class T>
  bool read(uint64_t off, T& out) const {
    if (!bytes(off, &out, sizeof(T))) return false;
    if (bigEndian && sizeof(T) > 1) {
      auto p = reinterpret_cast<uint8_t*>(&out);
      std::reverse(p, p + sizeof(T));
    }
    return true;
  }
  std::string cstr(uint64_t off, size_t maxLen = 256) const {
    char buf[257] = {};
    uint64_t end = std::min(size, limit);
    if (off >= end) return {};
    size_t n = (size_t)std::min<uint64_t>(maxLen, end - off);
    if (!bytes(off, buf, n)) return {};
    return std::string(buf, strnlen(buf, n));
  }
};

void noteExport(ModuleInfo& info, std::string_view name) {
  if (name == "VSTPluginMain") info.hasVstPluginMain = true;
  else if (name == "main" || name == "main_plugin" || name == "_main" || name == "MAIN") info.hasMain = true;
  else if (name == "GetPluginFactory") info.hasVst3Factory = true;
  else if (name == "clap_entry") info.hasClapEntry = true;
}

// ---- PE (Windows DLL) ----

struct Section {
  uint32_t va, vsize, rawPtr, rawSize;
};

bool rvaToOffset(const std::vector<Section>& secs, uint32_t rva, uint64_t& off) {
  for (auto& s : secs) {
    uint32_t size = s.vsize ? s.vsize : s.rawSize;
    if (rva >= s.va && rva < s.va + size) {
      off = uint64_t(rva - s.va) + s.rawPtr;
      return true;
    }
  }
  return false;
}

void readPe(Reader& r, ModuleInfo& info) {
  r.limit = 64ull << 20;
  info.format = BinaryFormat::PE;
  uint32_t lfanew = 0;
  uint32_t sig = 0;
  if (!r.read(0x3C, lfanew) || !r.read(lfanew, sig) || sig != 0x00004550 /*PE\0\0*/) {
    info.error = "not a PE file";
    return;
  }
  uint64_t fh = uint64_t(lfanew) + 4;
  uint16_t machine = 0, numSections = 0, optSize = 0, characteristics = 0;
  if (!r.read(fh, machine) || !r.read(fh + 2, numSections) || !r.read(fh + 16, optSize) ||
      !r.read(fh + 18, characteristics)) {
    info.error = "truncated header";
    return;
  }
  switch (machine) {
    case 0x014C: info.arch = Arch::X86; break;
    case 0x8664: info.arch = Arch::X64; break;
    case 0xAA64: info.arch = Arch::Arm64; break;
    default: info.arch = Arch::Other; break;
  }
  info.archs = {info.arch};
  info.isLibrary = (characteristics & 0x2000 /*IMAGE_FILE_DLL*/) != 0;

  uint64_t opt = fh + 20;
  uint16_t magic = 0;
  if (!r.read(opt, magic)) {
    info.error = "truncated optional header";
    return;
  }
  uint32_t exportRva = 0, exportSize = 0, numDirs = 0;
  if (magic == 0x10B || magic == 0x20B) {
    // The whole optional header must be there, like the IMAGE_OPTIONAL_HEADER32/64 it is.
    uint64_t ohSize = magic == 0x10B ? 224 : 240;
    uint8_t probe = 0;
    if (!r.read(opt + ohSize - 1, probe)) return;
    uint64_t dirs = opt + (magic == 0x10B ? 96 : 112);
    r.read(dirs - 4, numDirs);
    if (numDirs > 0) {
      r.read(dirs, exportRva);
      r.read(dirs + 4, exportSize);
    }
  } else {
    info.error = "unknown optional header";
    return;
  }

  std::vector<Section> secs;
  uint64_t secOff = opt + optSize;
  for (int i = 0; i < numSections; ++i) {
    uint64_t s = secOff + uint64_t(i) * 40;
    Section sec{};
    if (!r.read(s + 8, sec.vsize) || !r.read(s + 12, sec.va) || !r.read(s + 16, sec.rawSize) ||
        !r.read(s + 20, sec.rawPtr))
      break;
    secs.push_back(sec);
  }
  info.valid = true;

  if (exportRva == 0 || exportSize == 0) return;
  uint64_t expOff;
  if (!rvaToOffset(secs, exportRva, expOff)) return;
  uint32_t numberOfNames = 0, addressOfNames = 0;
  if (!r.read(expOff + 24, numberOfNames) || !r.read(expOff + 32, addressOfNames)) return;
  uint64_t namesOff;
  if (!rvaToOffset(secs, addressOfNames, namesOff)) return;
  uint32_t count = std::min<uint32_t>(numberOfNames, 100000);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t nameRva = 0;
    if (!r.read(namesOff + uint64_t(i) * 4, nameRva)) break;
    uint64_t nOff;
    if (!rvaToOffset(secs, nameRva, nOff)) continue;
    noteExport(info, r.cstr(nOff));
  }
}

// ---- ELF (Linux shared object) ----

void readElf(Reader& r, ModuleInfo& info) {
  info.format = BinaryFormat::ELF;
  uint8_t ident[16] = {};
  if (!r.bytes(0, ident, 16)) {
    info.error = "truncated header";
    return;
  }
  const bool is64 = ident[4] == 2;
  r.bigEndian = ident[5] == 2;
  uint16_t type = 0, machine = 0;
  if (!r.read(16, type) || !r.read(18, machine)) {
    info.error = "truncated header";
    return;
  }
  switch (machine) {
    case 3: info.arch = Arch::X86; break;
    case 62: info.arch = Arch::X64; break;
    case 183: info.arch = Arch::Arm64; break;
    default: info.arch = Arch::Other; break;
  }
  info.archs = {info.arch};
  info.isLibrary = type == 3 /*ET_DYN*/;

  uint64_t shoff = 0;
  uint16_t shentsize = 0, shnum = 0;
  if (is64) {
    r.read(40, shoff);
    r.read(58, shentsize);
    r.read(60, shnum);
  } else {
    uint32_t o = 0;
    r.read(32, o);
    shoff = o;
    r.read(46, shentsize);
    r.read(48, shnum);
  }
  info.valid = true;
  if (!shoff || !shnum || shentsize < (is64 ? 64 : 40)) return;

  struct Sh {
    uint32_t type = 0, link = 0;
    uint64_t offset = 0, size = 0, entsize = 0;
  };
  auto section = [&](uint32_t i, Sh& s) {
    uint64_t b = shoff + uint64_t(i) * shentsize;
    if (is64) {
      return r.read(b + 4, s.type) && r.read(b + 24, s.offset) && r.read(b + 32, s.size) && r.read(b + 40, s.link) &&
             r.read(b + 56, s.entsize);
    }
    uint32_t off = 0, size = 0, ent = 0;
    bool ok = r.read(b + 4, s.type) && r.read(b + 16, off) && r.read(b + 20, size) && r.read(b + 24, s.link) &&
              r.read(b + 36, ent);
    s.offset = off;
    s.size = size;
    s.entsize = ent;
    return ok;
  };
  for (uint32_t i = 0; i < std::min<uint32_t>(shnum, 4096); ++i) {
    Sh sym;
    if (!section(i, sym) || sym.type != 11 /*SHT_DYNSYM*/) continue;
    Sh str;
    if (!section(sym.link, str)) return;
    const uint64_t ent = sym.entsize ? sym.entsize : (is64 ? 24 : 16);
    const uint64_t count = std::min<uint64_t>(sym.size / ent, 200000);
    for (uint64_t k = 1; k < count; ++k) {
      uint64_t b = sym.offset + k * ent;
      uint32_t name = 0;
      uint8_t st_info = 0, st_other = 0;
      uint16_t shndx = 0;
      if (is64) {
        if (!r.read(b, name) || !r.read(b + 4, st_info) || !r.read(b + 5, st_other) || !r.read(b + 6, shndx)) break;
      } else {
        if (!r.read(b, name) || !r.read(b + 12, st_info) || !r.read(b + 13, st_other) || !r.read(b + 14, shndx)) break;
      }
      int bind = st_info >> 4, kind = st_info & 15, vis = st_other & 3;
      if (shndx == 0 || (bind != 1 && bind != 2) || (kind != 1 && kind != 2) || (vis != 0 && vis != 3)) continue;
      if (name >= str.size) continue;
      noteExport(info, r.cstr(str.offset + name));
    }
    return;
  }
}

// ---- Mach-O (macOS) ----

Arch machArch(int32_t cpu) {
  switch ((uint32_t)cpu) {
    case 7: return Arch::X86;
    case 0x01000007: return Arch::X64;
    case 0x0100000C: return Arch::Arm64;
    default: return Arch::Other;
  }
}

// Reads one thin Mach-O image at `base`.
bool readMachSlice(Reader& r, uint64_t base, ModuleInfo& info, bool exports) {
  uint32_t magic = 0;
  r.bigEndian = false;
  if (!r.read(base, magic)) return false;
  bool is64 = magic == 0xFEEDFACF;
  if (magic != 0xFEEDFACE && magic != 0xFEEDFACF) return false;
  int32_t cpu = 0;
  uint32_t filetype = 0, ncmds = 0;
  if (!r.read(base + 4, cpu) || !r.read(base + 12, filetype) || !r.read(base + 16, ncmds)) return false;
  if (!exports) return true;
  info.arch = machArch(cpu);
  info.isLibrary = filetype == 8 /*MH_BUNDLE*/ || filetype == 6 /*MH_DYLIB*/;
  uint64_t cmd = base + (is64 ? 32 : 28);
  for (uint32_t i = 0; i < std::min<uint32_t>(ncmds, 4096); ++i) {
    uint32_t c = 0, size = 0;
    if (!r.read(cmd, c) || !r.read(cmd + 4, size) || size < 8) break;
    if (c == 2 /*LC_SYMTAB*/) {
      uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
      r.read(cmd + 8, symoff);
      r.read(cmd + 12, nsyms);
      r.read(cmd + 16, stroff);
      r.read(cmd + 20, strsize);
      const uint64_t ent = is64 ? 16 : 12;
      for (uint32_t k = 0; k < std::min<uint32_t>(nsyms, 500000); ++k) {
        uint64_t b = base + symoff + uint64_t(k) * ent;
        uint32_t strx = 0;
        uint8_t ntype = 0;
        if (!r.read(b, strx) || !r.read(b + 4, ntype)) break;
        if (!(ntype & 0x01) || (ntype & 0x0E) != 0x0E || strx >= strsize) continue;  // N_EXT, N_SECT
        std::string name = r.cstr(base + stroff + strx);
        if (!name.empty() && name[0] == '_') noteExport(info, std::string_view(name).substr(1));
      }
    }
    cmd += size;
  }
  return true;
}

void readMachO(Reader& r, ModuleInfo& info) {
  info.format = BinaryFormat::MachO;
  uint32_t magic = 0;
  r.bigEndian = true;
  r.read(0, magic);
  if (magic == 0xCAFEBABE || magic == 0xCAFEBABF) {
    const bool fat64 = magic == 0xCAFEBABF;
    uint32_t n = 0;
    r.read(4, n);
    std::vector<std::pair<Arch, uint64_t>> slices;
    for (uint32_t i = 0; i < std::min<uint32_t>(n, 16); ++i) {
      r.bigEndian = true;
      uint64_t b = 8 + uint64_t(i) * (fat64 ? 32 : 20);
      int32_t cpu = 0;
      uint64_t off = 0;
      if (!r.read(b, cpu)) break;
      if (fat64) {
        r.read(b + 8, off);
      } else {
        uint32_t o = 0;
        r.read(b + 8, o);
        off = o;
      }
      slices.emplace_back(machArch(cpu), off);
      info.archs.push_back(machArch(cpu));
    }
    if (slices.empty()) {
      info.error = "empty universal binary";
      return;
    }
    auto pick = slices.front();
    for (auto& s : slices)
      if (s.first == nativeArch()) pick = s;
    info.valid = readMachSlice(r, pick.second, info, true);
    if (!info.valid) info.error = "bad universal binary";
    return;
  }
  info.valid = readMachSlice(r, 0, info, true);
  info.archs = {info.arch};
  if (!info.valid) info.error = "truncated header";
}

// A macOS .vst bundle: the executable named in Info.plist, else the only file in Contents/MacOS.
std::string bundleExecutable(const std::string& bundle) {
  std::string macos = joinPath(joinPath(bundle, "Contents"), "MacOS");
  std::string plist;
  if (readFile(joinPath(joinPath(bundle, "Contents"), "Info.plist"), plist)) {
    auto k = plist.find("<key>CFBundleExecutable</key>");
    if (k != std::string::npos) {
      auto a = plist.find("<string>", k);
      auto b = plist.find("</string>", a);
      if (a != std::string::npos && b != std::string::npos) {
        std::string exe = joinPath(macos, plist.substr(a + 8, b - a - 8));
        if (fileExists(exe)) return exe;
      }
    }
  }
  std::vector<DirEntry> list;
  if (listDir(macos, list))
    for (auto& e : list)
      if (!e.isDir) return joinPath(macos, e.name);
  return {};
}

}  // namespace

ModuleInfo readModuleInfo(const std::string& pathIn) {
  ModuleInfo info;
  std::string path = pathIn;
  if (dirExists(path)) {
    path = bundleExecutable(path);
    if (path.empty()) {
      info.error = "bundle without an executable";
      return info;
    }
  }
  Reader r;
  if (!r.open(path)) {
    info.error = "cannot open file";
    return info;
  }
  uint8_t m[4] = {};
  r.bytes(0, m, 4);
  if (m[0] == 'M' && m[1] == 'Z') {
    readPe(r, info);
  } else if (m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') {
    readElf(r, info);
  } else if ((m[0] == 0xCA && m[1] == 0xFE && m[2] == 0xBA && (m[3] == 0xBE || m[3] == 0xBF)) ||
             (m[0] == 0xCE && m[1] == 0xFA && m[2] == 0xED && m[3] == 0xFE) ||
             (m[0] == 0xCF && m[1] == 0xFA && m[2] == 0xED && m[3] == 0xFE)) {
    readMachO(r, info);
  } else {
    switch (nativeFormat()) {
      case BinaryFormat::PE: info.error = "not a PE file"; break;
      case BinaryFormat::MachO: info.error = "not a Mach-O file"; break;
      default: info.error = "not an ELF file"; break;
    }
  }
  return info;
}

}  // namespace yov
