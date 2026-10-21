#include "common/PluginDb.h"

#include <algorithm>
#include <thread>

namespace yov {

static const char* kStatusNames[] = {"unknown", "ok",         "warning",    "crashed",  "timeout",
                                     "load-failed", "not-plugin", "unsupported", "no-helper", "shell"};

const char* statusName(ScanStatus s) { return kStatusNames[(int)s]; }

ScanStatus statusFromName(const std::string& s) {
  for (int i = 0; i < (int)std::size(kStatusNames); ++i)
    if (s == kStatusNames[i]) return (ScanStatus)i;
  return ScanStatus::Unknown;
}

std::string PluginRecord::displayName() const {
  if (!name.empty()) return name;
  if (!product.empty()) return product;
  auto f = fileNameOf(path);
  auto dot = f.find_last_of('.');
  return dot == std::string::npos ? f : f.substr(0, dot);
}

std::string PluginRecord::key() const { return foldCase(path) + "|" + std::to_string(shellId); }

Json PluginRecord::toJson() const {
  Json j = Json::object();
  j["path"] = path;
  if (shellId) j["shellId"] = (int64_t)shellId;
  j["size"] = stamp.size;
  j["mtime"] = std::to_string(stamp.mtime);  // 64-bit: keep exact
  j["arch"] = arch;
  j["status"] = statusName(status);
  if (!detail.empty()) j["detail"] = detail;
  if (!stage.empty()) j["stage"] = stage;
  if (!warnings.empty()) {
    Json w = Json::array();
    for (auto& s : warnings) w.push(s);
    j["warnings"] = std::move(w);
  }
  if (!editor.empty()) j["editor"] = editor;
  j["name"] = name;
  j["vendor"] = vendor;
  j["product"] = product;
  j["uniqueId"] = (int64_t)uniqueId;
  j["version"] = (int64_t)version;
  j["vstVersion"] = (int64_t)vstVersion;
  j["category"] = (int64_t)category;
  j["isSynth"] = isSynth;
  j["hasEditor"] = hasEditor;
  j["usesChunks"] = usesChunks;
  j["midiInput"] = midiInput;
  j["numInputs"] = (int64_t)numInputs;
  j["numOutputs"] = (int64_t)numOutputs;
  j["numParams"] = (int64_t)numParams;
  j["numPrograms"] = (int64_t)numPrograms;
  j["cpuPercent"] = cpuPercent;
  j["scannedAt"] = scannedAt;
  j["scanMs"] = (int64_t)scanMs;
  return j;
}

PluginRecord PluginRecord::fromJson(const Json& j) {
  PluginRecord r;
  r.path = j.get("path").asString();
  r.shellId = (int32_t)j.get("shellId").asInt();
  r.stamp.size = (uint64_t)j.get("size").asInt();
  r.stamp.mtime = std::strtoull(j.get("mtime").asString().c_str(), nullptr, 10);
  r.arch = j.get("arch").asString();
  r.status = statusFromName(j.get("status").asString());
  r.detail = j.get("detail").asString();
  r.stage = j.get("stage").asString();
  for (auto& w : j.get("warnings").elements()) r.warnings.push_back(w.asString());
  r.editor = j.get("editor").asString();
  if (!j.has("editor") && (r.stage == "editor" || r.stage == "editor-close")) {
    // Scanned before the editor result was recorded separately: the stage says it.
    r.editor = r.status == ScanStatus::Timeout ? "hangs" : "crashed";
  }
  r.name = j.get("name").asString();
  r.vendor = j.get("vendor").asString();
  r.product = j.get("product").asString();
  r.uniqueId = (int32_t)j.get("uniqueId").asInt();
  r.version = (int32_t)j.get("version").asInt();
  r.vstVersion = (int32_t)j.get("vstVersion").asInt();
  r.category = (int32_t)j.get("category").asInt();
  r.isSynth = j.get("isSynth").asBool();
  r.hasEditor = j.get("hasEditor").asBool();
  r.usesChunks = j.get("usesChunks").asBool();
  r.midiInput = j.get("midiInput").asBool();
  r.numInputs = (int32_t)j.get("numInputs").asInt();
  r.numOutputs = (int32_t)j.get("numOutputs").asInt();
  r.numParams = (int32_t)j.get("numParams").asInt();
  r.numPrograms = (int32_t)j.get("numPrograms").asInt();
  r.cpuPercent = j.get("cpuPercent").asNumber();
  r.scannedAt = (uint64_t)j.get("scannedAt").asInt();
  r.scanMs = (uint32_t)j.get("scanMs").asInt();
  return r;
}

// ---------------------------------------------------------------------------------------------

std::string Settings::filePath() { return joinPath(appDataDir(), "settings.json"); }

void Settings::load() {
  std::string text;
  Json j;
  if (!readFile(filePath(), text) || !Json::parse(text, j) || !j.isObject()) return;
  folders.clear();
  for (auto& f : j.get("folders").elements())
    if (!f.asString().empty()) folders.push_back(f.asString());
  testEditors = j.get("testEditors").asBool(testEditors);
  hiddenDesktop = j.get("hiddenDesktop").asBool(hiddenDesktop);
  timeoutSec = (int)std::clamp<int64_t>(j.get("timeoutSec").asInt(timeoutSec), 5, 600);
  parallel = (int)std::clamp<int64_t>(j.get("parallel").asInt(parallel), 1, 16);
}

bool Settings::save() const {
  Json j = Json::object();
  Json f = Json::array();
  for (auto& s : folders) f.push(s);
  j["folders"] = std::move(f);
  j["testEditors"] = testEditors;
  j["hiddenDesktop"] = hiddenDesktop;
  j["timeoutSec"] = timeoutSec;
  j["parallel"] = parallel;
  return writeFileAtomic(filePath(), j.dump(true));
}

// ---------------------------------------------------------------------------------------------

std::string PluginDb::defaultPath() { return joinPath(appDataDir(), "plugins.json"); }

bool PluginDb::load() {
  std::string text;
  FileStamp st;
  fileStamp(filePath(), st);
  if (!readFile(filePath(), text)) {
    std::lock_guard<std::mutex> lk(m_);
    diskStamp_ = st;
    return false;
  }
  Json j;
  if (!Json::parse(text, j)) return false;
  std::vector<PluginRecord> recs;
  for (auto& e : j.get("plugins").elements()) {
    auto r = PluginRecord::fromJson(e);
    if (!r.path.empty()) recs.push_back(std::move(r));
  }
  std::lock_guard<std::mutex> lk(m_);
  recs_ = std::move(recs);
  diskStamp_ = st;
  return true;
}

bool PluginDb::save() {
  Json j = Json::object();
  j["version"] = 1;
  Json arr = Json::array();
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& r : recs_) arr.push(r.toJson());
  }
  j["plugins"] = std::move(arr);
  bool ok = false;
  // Another instance may be writing at the same moment; retry briefly.
  for (int i = 0; i < 5 && !ok; ++i) {
    ok = writeFileAtomic(filePath(), j.dump(true));
    if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  FileStamp st;
  fileStamp(filePath(), st);
  std::lock_guard<std::mutex> lk(m_);
  diskStamp_ = st;
  return ok;
}

bool PluginDb::reloadIfChanged() {
  FileStamp st;
  if (!fileStamp(filePath(), st)) return false;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (st == diskStamp_) return false;
  }
  return load();
}

std::vector<PluginRecord> PluginDb::all() const {
  std::lock_guard<std::mutex> lk(m_);
  return recs_;
}

bool PluginDb::find(const std::string& path, int32_t shellId, PluginRecord& out) const {
  auto lp = foldCase(path);
  std::lock_guard<std::mutex> lk(m_);
  for (auto& r : recs_)
    if (r.shellId == shellId && foldCase(r.path) == lp) {
      out = r;
      return true;
    }
  return false;
}

std::vector<PluginRecord> PluginDb::forPath(const std::string& path) const {
  auto lp = foldCase(path);
  std::vector<PluginRecord> out;
  std::lock_guard<std::mutex> lk(m_);
  for (auto& r : recs_)
    if (foldCase(r.path) == lp) out.push_back(r);
  return out;
}

bool PluginDb::findMoved(int32_t uniqueId, const std::string& name, PluginRecord& out) const {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& r : recs_)
    if (r.uniqueId == uniqueId && uniqueId != 0 && r.name == name && fileExists(r.path)) {
      out = r;
      return true;
    }
  return false;
}

void PluginDb::upsert(const PluginRecord& rec) {
  auto k = rec.key();
  std::lock_guard<std::mutex> lk(m_);
  for (auto& r : recs_)
    if (r.key() == k) {
      r = rec;
      return;
    }
  recs_.push_back(rec);
}

void PluginDb::removeShellChildren(const std::string& path) {
  auto lp = foldCase(path);
  std::lock_guard<std::mutex> lk(m_);
  recs_.erase(std::remove_if(recs_.begin(), recs_.end(),
                             [&](const PluginRecord& r) { return r.shellId != 0 && foldCase(r.path) == lp; }),
              recs_.end());
}

void PluginDb::prune(const std::vector<std::string>& folders) {
  std::vector<std::string> lf;
  for (auto& f : folders) {
    auto l = foldCase(f);
    if (!l.empty() && l.back() != '\\' && l.back() != '/') l += '\\';
    for (auto& c : l)
      if (c == '/') c = '\\';
    lf.push_back(l);
  }
  std::lock_guard<std::mutex> lk(m_);
  recs_.erase(std::remove_if(recs_.begin(), recs_.end(),
                             [&](const PluginRecord& r) {
                               auto lp = foldCase(r.path);
                               for (auto& c : lp)
                                 if (c == '/') c = '\\';
                               bool inside = false;
                               for (auto& f : lf)
                                 if (lp.rfind(f, 0) == 0) inside = true;
                               return !inside || !fileExists(r.path);
                             }),
              recs_.end());
}

void PluginDb::clear() {
  std::lock_guard<std::mutex> lk(m_);
  recs_.clear();
}

}  // namespace yov
