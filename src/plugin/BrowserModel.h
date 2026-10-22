// What the plugin browser shows, independent of the GUI toolkit: the filtered, grouped and
// sorted rows of the plugin list, their texts, colours and tooltips, and the status line. The
// Win32 GUI and the Dear ImGui GUI both draw from this, so they show the same thing.
#pragma once

#include "common/PluginDb.h"
#include "scan/Scanner.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace yov {

// The dark palette (0xRRGGBB).
namespace palette {
constexpr uint32_t kBg = 0x1C1D21;
constexpr uint32_t kPanel = 0x25272C;
constexpr uint32_t kCtrl = 0x2F3238;
constexpr uint32_t kCtrlHot = 0x3A3D45;
constexpr uint32_t kBorder = 0x40434A;
constexpr uint32_t kText = 0xE3E5E8;
constexpr uint32_t kDim = 0x959AA4;
constexpr uint32_t kAccent = 0xD9A441;  // old brass
constexpr uint32_t kAccentPressed = 0xB88A35;
constexpr uint32_t kAccentText = 0x1C1D21;
constexpr uint32_t kOk = 0x5CC76E;
constexpr uint32_t kWarn = 0xE8BB3A;
constexpr uint32_t kBad = 0xEB5B52;
constexpr uint32_t kGrey = 0x7C818A;
constexpr uint32_t kBlue = 0x6CB4EE;
constexpr uint32_t kRowAlt = 0x222429;
constexpr uint32_t kSelect = 0x4A3E24;
}  // namespace palette

enum Filter { F_ALL, F_WORKING, F_PROBLEMS, F_INSTRUMENTS, F_EFFECTS, F_32BIT, F_OTHER, F_COUNT };
extern const char* const kFilterNames[F_COUNT];

constexpr int kColumnCount = 8;
extern const char* const kColumnNames[kColumnCount];
extern const int kColumnWidths[kColumnCount];  // at 96 dpi

const char* statusLabel(ScanStatus s);
uint32_t statusColor(ScanStatus s);
std::string editorLabel(const PluginRecord& r);
// The usual VST2 folders of this system that exist.
std::vector<std::string> defaultVstFolders();
// What a folder picker / dialog title calls this system's plugin files.
const char* pluginFileKind();  // "VST2 plugins (.dll)" ...
// Name of the system file manager ("Explorer", "file manager", "Finder").
const char* fileManagerName();

class BrowserModel {
 public:
  struct Group {
    std::string key;
    std::vector<PluginRecord> recs;  // sorted by path
    int best = 0;                    // the copy "Load" picks
  };
  struct Row {
    int group;
    int member;  // -1: the header row of a group with several files
  };

  // Rebuilds the rows from the database. selectedId: rowId() of the selected row before; returns
  // where that row is now (-1 if it is gone).
  int rebuild(const std::vector<PluginRecord>& all, int filter, const std::string& search,
              const std::string& selectedId);

  int rowCount() const { return (int)rows_.size(); }
  std::string rowId(int row) const;
  // The record a row loads (the best copy for a group header).
  const PluginRecord* rowRecord(int row) const;
  bool rowIsGroup(int row) const;
  bool rowExpanded(int row) const;
  bool rowIsChild(int row) const;  // one file of an expanded group
  const Group* rowGroup(int row) const;
  // Expands / collapses a group header row (rebuild afterwards).
  void toggle(int row);

  std::string cellText(int row, int col) const;
  uint32_t cellColor(int row, int col) const;
  std::string tooltip(int row) const;

  int sortColumn = 0;
  bool sortAscending = true;
  std::set<std::string> expanded;

 private:
  std::vector<Group> groups_;
  std::vector<Row> rows_;
};

// The line under the plugin list: scan progress, the last scan's result, or a summary.
std::string scanStatusLine(const ScanProgress& p, const PluginDb& db);

// Before loading a plugin whose test went wrong: the question to ask. Empty text: just load.
struct LoadQuestion {
  std::string text;
  // false: OK / Cancel (it runs bridged anyway). true: Yes = sandboxed, No = direct, Cancel.
  bool offerSandbox = false;
};
LoadQuestion loadQuestion(const PluginRecord& r, bool sandboxed);

}  // namespace yov
