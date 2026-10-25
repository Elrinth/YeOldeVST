// The YeOldeVST CLAP plugin (also exposed as VST3 through clap-wrapper).
//
// Two flavours share this class: an instrument (notes in, stereo out) and an effect (stereo in,
// notes in, stereo out). Each instance hosts at most one VST 1.x/2.x plugin.
#pragma once

#include "common/PluginDb.h"
#include "plugin/HostedPlugin.h"
#include "plugin/clap-program-list.h"
#include "scan/Scanner.h"

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace yov {

class Gui;

// Fixed number of parameter slots shown to the DAW. Unused slots are hidden.
constexpr uint32_t kMaxParams = 1024;
// Parameter slots shown while no plugin is loaded (for hosts that never re-read the list).
constexpr uint32_t kGenericParams = 256;
// Stereo output pairs of the multi-out instrument (Kontakt "16out" style plugins).
constexpr uint32_t kMultiOutPairs = 16;

enum class Flavour { Instrument, Effect, InstrumentMulti };

// One process-wide set of settings / database / scanner shared by every instance.
struct Shared {
  Settings settings;
  PluginDb db;
  Scanner scanner{db};
  static Shared& get();
  static Shared* existing();  // null if nothing created it yet
};

class Plugin final : public HostedPlugin::Listener {
 public:
  Plugin(const clap_host_t* host, Flavour flavour);
  ~Plugin() override;

  static const clap_plugin_descriptor_t* descriptor(Flavour flavour);
  const clap_plugin_t* clapPlugin() const { return &plugin_; }

  // ---- used by the GUI (main thread) ----
  bool isInstrument() const { return instrument_; }
  HostedPlugin* hosted() const { return slot_ ? slot_->plugin.get() : nullptr; }
  const PluginRecord& loadedRecord() const { return loaded_; }
  bool hasMissingPlugin() const { return !missingState_.empty(); }
  const std::string& missingDescription() const { return missingText_; }

  bool loadVst(const PluginRecord& rec, bool sandboxed, std::string& error);
  void unloadVst();
  // After a crash: start a fresh instance and restore the last known good state.
  bool reload(std::string& error);
  void setProgram(int32_t index);
  // The hosted plugin's programs, as the host sees them (clap-program-list.h): read again after a load, a
  // state, or a display update (at most once a second then); the host is told when they changed.
  const std::vector<std::string>& programNames() const { return programNames_; }
  void editParameter(uint32_t index, double value);  // from the generic editor
  void beginParameterEdit(uint32_t index);
  void endParameterEdit(uint32_t index);
  const std::vector<std::string>& paramNames() const { return paramNames_; }
  bool genericView() const { return genericView_; }
  void setGenericView(bool g) { genericView_ = g; }
  void requestResize(uint32_t w, uint32_t h);
  const clap_host_t* host() const { return host_; }
  const clap_host_timer_support_t* hostTimer() const { return hostTimer_; }
  const clap_host_posix_fd_support_t* hostFd() const { return hostFd_; }
  bool isMainThread() const;

  // ---- HostedPlugin::Listener (from the VST2 plugin, any thread) ----
  void vstAutomate(int32_t index, float value) override;
  void vstBeginEdit(int32_t index) override;
  void vstEndEdit(int32_t index) override;
  bool vstSizeWindow(int32_t width, int32_t height) override;
  void vstIOChanged() override;
  void vstUpdateDisplay() override;
  void vstMidiOut(const vst2::VstEvents* events) override;

 private:
  struct Slot {
    std::unique_ptr<HostedPlugin> plugin;
    std::vector<std::vector<float>> in, out;
    std::vector<float*> inPtr, outPtr;
    void allocate(uint32_t frames);
  };
  struct OutEvent {
    enum Kind : uint8_t { Value, Begin, End } kind;
    uint32_t index;
    double value;
  };

  // CLAP plugin
  bool init();
  void destroy();
  bool activate(double sr, uint32_t minFrames, uint32_t maxFrames);
  void deactivate();
  bool startProcessing();
  void stopProcessing();
  void reset();
  clap_process_status process(const clap_process_t* p);
  const void* getExtension(const char* id);
  void onMainThread();

  // Extensions
  uint32_t paramsCount();
  bool paramsInfo(uint32_t index, clap_param_info_t* info);
  bool paramsValue(clap_id id, double* value);
  bool paramsValueToText(clap_id id, double value, char* out, uint32_t size);
  bool paramsTextToValue(clap_id id, const char* text, double* value);
  void paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out);
  bool stateSave(const clap_ostream_t* stream);
  bool stateLoad(const clap_istream_t* stream);
  uint32_t latency();
  uint32_t programCount();
  bool programName(uint32_t index, char* name, uint32_t capacity);
  int32_t programCurrent();
  bool programSelect(uint32_t index);
  void refreshPrograms(bool notify);

  // Helpers
  void handleInputEvents(const clap_input_events_t* in, Slot* slot, uint32_t frames);
  void emitOutputEvents(const clap_output_events_t* out);
  void queueOut(OutEvent e);
  void rebuildParamNames();
  void notifyParamsChanged(bool full);
  std::string buildStateJson();
  bool applyStateJson(const std::string& json, std::string& error);
  void installSlot(std::unique_ptr<Slot> s);  // swaps under the process lock; old one destroyed here
  void updateTime(const clap_event_transport_t* t, uint32_t frames);
  // Sets a parameter on behalf of the host, remembering the value to recognise the plugin's echo.
  void hostSetParameter(HostedPlugin* h, uint32_t index, double value);
  struct HostSet {
    std::atomic<float> value{-1.f};
    std::atomic<uint32_t> ms{0};
  };
  HostSet hostSet_[kMaxParams];

  const clap_host_t* host_;
  const clap_host_params_t* hostParams_ = nullptr;
  const clap_host_latency_t* hostLatency_ = nullptr;
  const clap_host_state_t* hostState_ = nullptr;
  const clap_host_gui_t* hostGui_ = nullptr;
  const clap_host_thread_check_t* hostThreadCheck_ = nullptr;
  const clap_host_timer_support_t* hostTimer_ = nullptr;
  const clap_host_posix_fd_support_t* hostFd_ = nullptr;
  const clap_host_program_list_t* hostPrograms_ = nullptr;
  bool instrument_;
  uint32_t outPairs_ = 1;
  bool wrappedAsVst3_ = false;
  uint64_t mainThreadId_ = 0;
  clap_plugin_t plugin_;

  double sampleRate_ = 44100;
  uint32_t maxFrames_ = 1024;
  bool active_ = false;
  bool initializing_ = false;

  std::mutex procLock_;  // audio thread try_locks; main thread locks to swap the slot
  std::unique_ptr<Slot> slot_;
  PluginRecord loaded_;
  bool loadedSandboxed_ = false;
  std::string lastGoodState_;  // JSON, for reload after a crash
  std::string missingState_;   // state of a plugin we could not find, kept verbatim
  std::string missingText_;
  std::vector<std::string> paramNames_;
  std::vector<std::string> programNames_;
  uint64_t programsReadMs_ = 0;
  bool genericView_ = false;

  // MIDI events for the current block
  std::vector<uint8_t> eventStore_;
  std::vector<vst2::VstMidiEvent> midiEvents_;
  std::vector<vst2::VstMidiSysexEvent> sysexEvents_;
  std::vector<std::vector<char>> sysexData_;
  vst2::VstTimeInfo time_{};
  bool wasPlaying_ = false;
  double samplePosCounter_ = 0;

  std::mutex outLock_;
  std::vector<OutEvent> outQueue_;

  // MIDI the hosted plugin sends out (audioMasterProcessEvents), passed to the host after
  // process(). Sysex payloads live in midiOutSysex_ until the block is done.
  struct MidiOut {
    uint32_t time;
    uint8_t bytes[3];
    int32_t sysex;  // index into midiOutSysex_, -1 for short messages
  };
  std::mutex midiOutLock_;
  std::vector<MidiOut> midiOut_;
  std::vector<std::vector<uint8_t>> midiOutSysex_;
  std::vector<MidiOut> midiOutBlock_;
  std::vector<std::vector<uint8_t>> midiOutSysexBlock_;
  void emitMidiOut(const clap_output_events_t* out, uint32_t frames);

  std::atomic<bool> pendingCrash_{false}, pendingIO_{false}, pendingDisplay_{false};
  std::atomic<uint32_t> pendingSize_{0};
  bool crashReported_ = false;

  std::unique_ptr<Gui> gui_;
  friend class Gui;
  friend struct PluginGlue;
};

}  // namespace yov
