#include "plugin/BrowserModel.h"

#include "common/Util.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace yov {

const char* const kFilterNames[F_COUNT] = {"All plugins", "Working",     "Problems",        "Instruments",
                                           "Effects",     "32-bit only", "Not VST2 / other"};

const char* const kColumnNames[kColumnCount] = {"Name",   "Vendor", "Type",    "Bits",
                                                "Status", "Editor", "Details", "File"};
const int kColumnWidths[kColumnCount] = {230, 130, 90, 52, 76, 74, 280, 360};

const char* statusLabel(ScanStatus s) {
  switch (s) {
    case ScanStatus::Ok: return "OK";
    case ScanStatus::Warning: return "Warning";
    case ScanStatus::Crashed: return "Crashed";
    case ScanStatus::Timeout: return "Hangs";
    case ScanStatus::LoadFailed: return "Won't load";
    case ScanStatus::NotPlugin: return "Not VST2";
    case ScanStatus::Unsupported: return "Unsupported";
    case ScanStatus::NoHelper: return "Needs helper";
    case ScanStatus::Shell: return "Shell";
    default: return "Not scanned";
  }
}

uint32_t statusColor(ScanStatus s) {
  switch (s) {
    case ScanStatus::Ok: return palette::kOk;
    case ScanStatus::Warning: return palette::kWarn;
    case ScanStatus::Crashed:
    case ScanStatus::Timeout:
    case ScanStatus::LoadFailed: return palette::kBad;
    case ScanStatus::NoHelper: return palette::kBlue;
    default: return palette::kGrey;
  }
}

std::string editorLabel(const PluginRecord& r) {
  if (r.editor == "ok") return "OK";
  if (r.editor == "none" || (!r.hasEditor && statusLoadable(r.status))) return "\xE2\x80\x94";  // em dash
  if (r.editor == "crashed") return "Crashed";
  if (r.editor == "hangs") return "Hangs";
  if (r.editor == "failed") return "Failed";
  return (r.hasEditor && statusLoadable(r.status)) ? "not tested" : "";
}

std::vector<std::string> defaultVstFolders() {
  std::vector<std::string> out;
  auto add = [&](const std::string& p) {
    if (p.empty() || !dirExists(p)) return;
    for (auto& o : out)
      if (pathKey(o) == pathKey(p)) return;
    out.push_back(p);
  };
#if defined(_WIN32)
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
      HKEY k;
      if (RegOpenKeyExW(root, L"SOFTWARE\\VST", 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024];
        DWORD sz = sizeof buf, type = 0;
        if (RegQueryValueExW(k, L"VSTPluginsPath", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
          buf[std::min<DWORD>(sz / 2, 1023)] = 0;
          add(trim(narrow(buf)));
        }
        RegCloseKey(k);
      }
    }
  }
  for (const char* p : {"C:\\Program Files\\VstPlugins", "C:\\Program Files\\Steinberg\\VstPlugins",
                        "C:\\Program Files\\Common Files\\VST2", "C:\\Program Files\\Common Files\\Steinberg\\VST2",
                        "C:\\Program Files (x86)\\VstPlugins", "C:\\Program Files (x86)\\Steinberg\\VstPlugins",
                        "C:\\Program Files (x86)\\Common Files\\VST2"})
    add(p);
#else
  std::string vstPath;
  if (getEnv("VST_PATH", vstPath)) {
    size_t start = 0;
    while (start <= vstPath.size()) {
      size_t colon = vstPath.find(':', start);
      add(vstPath.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
      if (colon == std::string::npos) break;
      start = colon + 1;
    }
  }
  std::string home;
  getEnv("HOME", home);
#if defined(__APPLE__)
  add(joinPath(home, "Library/Audio/Plug-Ins/VST"));
  add("/Library/Audio/Plug-Ins/VST");
#else
  for (const char* p : {".vst", ".lxvst"}) add(joinPath(home, p));
  for (const char* p : {"/usr/local/lib/vst", "/usr/local/lib/lxvst", "/usr/lib/vst", "/usr/lib/lxvst",
                        "/usr/lib64/vst", "/usr/lib64/lxvst"})
    add(p);
#endif
#endif
  return out;
}

const char* pluginFileKind() {
#if defined(_WIN32)
  return "VST2 plugins (.dll)";
#elif defined(__APPLE__)
  return "VST2 plugins (.vst)";
#else
  return "VST2 plugins (.so)";
#endif
}

const char* fileManagerName() {
#if defined(_WIN32)
  return "Explorer";
#elif defined(__APPLE__)
  return "Finder";
#else
  return "file manager";
#endif
}

// ---------------------------------------------------------------------------------------------

int BrowserModel::rebuild(const std::vector<PluginRecord>& all, int filter, const std::string& searchIn,
                          const std::string& selId) {
  std::string search = foldCase(trim(searchIn));
  std::vector<PluginRecord> shown;
  for (auto& r : all) {
    if (r.status == ScanStatus::Shell) continue;
    bool other = r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported;
    switch (filter) {
      case F_ALL: if (other) continue; break;
      case F_WORKING: if (!statusLoadable(r.status)) continue; break;
      case F_PROBLEMS: if (!statusAngry(r.status) && r.status != ScanStatus::Warning && r.status != ScanStatus::NoHelper) continue; break;
      case F_INSTRUMENTS: if (other || !r.isInstrument()) continue; break;
      case F_EFFECTS: if (other || r.isInstrument() || r.status == ScanStatus::LoadFailed) continue; break;
      case F_32BIT: if (other || r.arch != "x86") continue; break;
      case F_OTHER: if (!other) continue; break;
    }
    if (!search.empty()) {
      std::string hay = foldCase(r.displayName() + " " + r.vendor + " " + r.path);
      if (hay.find(search) == std::string::npos) continue;
    }
    shown.push_back(r);
  }

  // Group copies of the same plugin (same name + vendor). Files that aren't plugins stay alone.
  groups_.clear();
  std::map<std::string, size_t> index;
  for (auto& r : shown) {
    bool other = r.status == ScanStatus::NotPlugin || r.status == ScanStatus::Unsupported;
    std::string key = other ? "file|" + pathKey(r.path) + "|" + std::to_string(r.shellId)
                            : foldCase(r.displayName()) + "|" + foldCase(r.vendor);
    auto it = index.find(key);
    if (it == index.end()) {
      index[key] = groups_.size();
      groups_.push_back({key, {r}, 0});
    } else {
      groups_[it->second].recs.push_back(r);
    }
  }
  // The copy "Load" uses: working beats warning beats the rest; native (64-bit) beats 32-bit.
  const std::string native = archName(nativeArch());
  auto rank = [&](const PluginRecord& r) {
    int s = r.status == ScanStatus::Ok ? 0 : r.status == ScanStatus::Warning ? 2 : 10 + (int)r.status;
    return s + (r.arch == native ? 0 : 1);
  };
  for (auto& g : groups_) {
    std::sort(g.recs.begin(), g.recs.end(),
              [](const PluginRecord& a, const PluginRecord& b) { return foldCase(a.path) < foldCase(b.path); });
    for (size_t i = 1; i < g.recs.size(); ++i)
      if (rank(g.recs[i]) < rank(g.recs[g.best])) g.best = (int)i;
  }

  auto key = [&](const Group& g) -> std::string {
    const PluginRecord& r = g.recs[g.best];
    switch (sortColumn) {
      case 1: return foldCase(r.vendor);
      case 2: return r.isInstrument() ? "a" : "b";
      case 3: return r.arch;
      case 4: return std::to_string((int)r.status);
      case 5: return r.editor;
      case 6: return foldCase(r.detail);
      case 7: return foldCase(r.path);
      default: return foldCase(r.displayName());
    }
  };
  std::stable_sort(groups_.begin(), groups_.end(), [&](const Group& a, const Group& b) {
    auto ka = key(a), kb = key(b);
    if (ka == kb) return foldCase(a.recs[a.best].displayName()) < foldCase(b.recs[b.best].displayName());
    return sortAscending ? ka < kb : ka > kb;
  });

  rows_.clear();
  int newSel = -1;
  for (int g = 0; g < (int)groups_.size(); ++g) {
    auto id = [&](int m) { return groups_[g].key + "#" + (m < 0 ? std::string() : groups_[g].recs[m].key()); };
    if (groups_[g].recs.size() == 1) {
      if (id(0) == selId) newSel = (int)rows_.size();
      rows_.push_back({g, 0});
      continue;
    }
    if (id(-1) == selId) newSel = (int)rows_.size();
    rows_.push_back({g, -1});
    if (expanded.count(groups_[g].key))
      for (int m = 0; m < (int)groups_[g].recs.size(); ++m) {
        if (id(m) == selId) newSel = (int)rows_.size();
        rows_.push_back({g, m});
      }
  }
  return newSel;
}

std::string BrowserModel::rowId(int row) const {
  if (row < 0 || row >= (int)rows_.size()) return {};
  auto& g = groups_[rows_[row].group];
  return g.key + "#" + (rows_[row].member < 0 ? std::string() : g.recs[rows_[row].member].key());
}

const PluginRecord* BrowserModel::rowRecord(int row) const {
  if (row < 0 || row >= (int)rows_.size()) return nullptr;
  auto& g = groups_[rows_[row].group];
  return &g.recs[rows_[row].member < 0 ? g.best : rows_[row].member];
}

bool BrowserModel::rowIsGroup(int row) const { return row >= 0 && row < (int)rows_.size() && rows_[row].member < 0; }

bool BrowserModel::rowExpanded(int row) const {
  return rowIsGroup(row) && expanded.count(groups_[rows_[row].group].key) != 0;
}

bool BrowserModel::rowIsChild(int row) const {
  return row >= 0 && row < (int)rows_.size() && rows_[row].member >= 0 && groups_[rows_[row].group].recs.size() > 1;
}

const BrowserModel::Group* BrowserModel::rowGroup(int row) const {
  if (row < 0 || row >= (int)rows_.size()) return nullptr;
  return &groups_[rows_[row].group];
}

void BrowserModel::toggle(int row) {
  if (!rowIsGroup(row)) return;
  auto& key = groups_[rows_[row].group].key;
  if (!expanded.erase(key)) expanded.insert(key);
}

std::string BrowserModel::cellText(int row, int col) const {
  if (row < 0 || row >= (int)rows_.size()) return {};
  const Group& g = groups_[rows_[row].group];
  const bool header = rows_[row].member < 0;
  const bool child = !header && g.recs.size() > 1;
  const PluginRecord& rec = *rowRecord(row);
  const bool other = rec.status == ScanStatus::NotPlugin || rec.status == ScanStatus::LoadFailed ||
                     rec.status == ScanStatus::Unsupported;
  switch (col) {
    case 0:
      if (header) return (expanded.count(g.key) ? "\xE2\x96\xBE  " : "\xE2\x96\xB8  ") + rec.displayName();  // ▾ ▸
      if (child) return "        " + fileNameOf(rec.path);
      return rec.displayName();
    case 1: return child ? std::string() : rec.vendor;
    case 2: return other || child ? std::string() : (rec.isInstrument() ? "Instrument" : "Effect");
    case 3: {
      if (header) {
        bool b32 = false, b64 = false;
        for (auto& r : g.recs) (r.arch == "x86" ? b32 : b64) = true;
        return b32 && b64 ? "32+64" : b32 ? "32" : "64";
      }
      return rec.arch == "x86" ? "32" : rec.arch == "x64" ? "64" : rec.arch;
    }
    case 4: return statusLabel(rec.status);
    case 5: {
      if (!header) return editorLabel(rec);
      std::string first;
      bool same = true, any = false;
      for (auto& r : g.recs) {
        if (!statusLoadable(r.status)) continue;
        auto l = editorLabel(r);
        if (!any) first = l;
        else if (l != first) same = false;
        any = true;
      }
      return any ? (same ? first : "mixed") : std::string();
    }
    case 6: {
      if (!header) return rec.detail;
      int ok = 0, bad = 0;
      for (auto& r : g.recs) {
        if (statusLoadable(r.status)) ++ok;
        else ++bad;
      }
      char b[128];
      snprintf(b, sizeof b, "%d files: %d work, %d with problems", (int)g.recs.size(), ok, bad);
      return b;
    }
    case 7: return header ? dirOf(rec.path) : rec.path;
  }
  return {};
}

uint32_t BrowserModel::cellColor(int row, int col) const {
  const PluginRecord* r = rowRecord(row);
  if (!r) return palette::kText;
  if (r->status == ScanStatus::NotPlugin || r->status == ScanStatus::Unsupported) return palette::kGrey;
  if (col == 4) return statusColor(r->status);
  if (col == 5) {
    auto t = cellText(row, 5);
    return t == "OK"                                             ? palette::kOk
           : (t == "Crashed" || t == "Hangs" || t == "Failed") ? palette::kBad
           : t == "mixed"                                      ? palette::kWarn
                                                               : palette::kDim;
  }
  if (col == 3 && cellText(row, 3) != "64") return palette::kBlue;
  if (col >= 6 || col == 1 || col == 2) return palette::kDim;
  return palette::kText;
}

std::string BrowserModel::tooltip(int row) const {
  const PluginRecord* rp = rowRecord(row);
  if (!rp) return {};
  if (rowIsGroup(row)) {
    // Group: one line per copy.
    auto& g = groups_[rows_[row].group];
    std::string t = rp->displayName() + " (" + std::to_string(g.recs.size()) + " files)";
    int shown = 0;
    for (auto& c : g.recs) {
      if (++shown > 20) {
        t += "\n...";
        break;
      }
      t += "\n" + std::string(c.arch == "x86" ? "32" : "64") + "-bit  " + statusLabel(c.status) + "  " +
           fileNameOf(c.path);
    }
    t += "\nDouble-click to show all copies; Load picks " + fileNameOf(rp->path);
    return t;
  }
  auto& rec = *rp;
  std::string t = rec.displayName();
  if (!rec.vendor.empty()) t += " by " + rec.vendor;
  t += "\n" + std::string(rec.arch == "x86" ? "32-bit" : "64-bit") + ", " + statusLabel(rec.status);
  if (!rec.detail.empty()) t += "\n" + rec.detail;
  for (size_t i = 1; i < rec.warnings.size(); ++i) t += "\n" + rec.warnings[i];
  if (rec.numParams) t += "\n" + std::to_string(rec.numParams) + " parameters";
  if (rec.numPrograms > 1) t += ", " + std::to_string(rec.numPrograms) + " presets";
  if (rec.cpuPercent > 0) {
    char b[64];
    snprintf(b, sizeof b, "\nCPU during test: %.1f%% of one core", rec.cpuPercent);
    t += b;
  }
  t += "\n" + rec.path;
  return t;
}

std::string scanStatusLine(const ScanProgress& p, const PluginDb& db) {
  const char* dot = "   \xE2\x80\xA2   ";  // bullet
  if (p.running) {
    char b[256];
    snprintf(b, sizeof b, "Testing %d of %d%s%d OK%s%d with problems", p.done, p.total, dot, p.ok, dot, p.angry);
    std::string s = b;
    if (!p.current.empty()) s += dot + fileNameOf(p.current.front());
    return s;
  }
  if (!p.message.empty()) return p.message;
  int ok = 0, bad = 0;
  for (auto& r : db.all()) {
    if (statusLoadable(r.status)) ++ok;
    if (statusAngry(r.status)) ++bad;
  }
  char b[200];
  snprintf(b, sizeof b, "%d plugins work, %d have problems. Double-click a plugin to load it.", ok, bad);
  return b;
}

LoadQuestion loadQuestion(const PluginRecord& r, bool sandboxed) {
  LoadQuestion q;
  bool bridged = sandboxed || r.arch == "x86";
  if (!statusAngry(r.status) && r.status != ScanStatus::Unknown) return q;
  q.text = r.displayName() + " had a problem when it was tested:\n\n" +
           (r.detail.empty() ? std::string("it has not been tested yet") : r.detail) + "\n\n";
  q.text += bridged ? "It will run in a separate process, so if it crashes your DAW keeps running. Load it anyway?"
                    : "Loading it straight into your DAW could crash the DAW. Load it sandboxed (in a separate "
                      "process) instead?\n\nYes = sandboxed, No = load directly anyway, Cancel = don't load";
  q.offerSandbox = !bridged;
  return q;
}

}  // namespace yov
