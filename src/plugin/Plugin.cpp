#include "plugin/Plugin.h"

#include "common/Json.h"
#include "common/Util.h"
#include "plugin/Gui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace vst2;

namespace yov {

static const char* kVersion = "1.3.0";

static std::atomic<Shared*> gShared{nullptr};

Shared& Shared::get() {
  static Shared* s = [] {
    auto* sh = new Shared();  // never destroyed: plugin DLL unload order is up to the host
    sh->settings.load();
    sh->db.load();
    gShared = sh;
    return sh;
  }();
  return *s;
}

Shared* Shared::existing() { return gShared.load(); }

// ---------------------------------------------------------------------------------------------
// Descriptors

static const char* kInstrumentFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                            CLAP_PLUGIN_FEATURE_STEREO, nullptr};
static const char* kEffectFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

static const clap_plugin_descriptor_t kInstrumentDesc = {
    CLAP_VERSION_INIT,
    "com.thefirstboss.yeoldevst.instrument",
    "YeOldeVST Instrument",
    "Elrinth",
    "https://github.com/Elrinth/YeOldeVST",
    "",
    "",
    kVersion,
    "Hosts old VST 1.x/2.x instruments (32-bit and 64-bit)",
    kInstrumentFeatures,
};

static const clap_plugin_descriptor_t kEffectDesc = {
    CLAP_VERSION_INIT,
    "com.thefirstboss.yeoldevst.effect",
    "YeOldeVST Effect",
    "Elrinth",
    "https://github.com/Elrinth/YeOldeVST",
    "",
    "",
    kVersion,
    "Hosts old VST 1.x/2.x effects (32-bit and 64-bit)",
    kEffectFeatures,
};

static const clap_plugin_descriptor_t kInstrumentMultiDesc = {
    CLAP_VERSION_INIT,
    "com.thefirstboss.yeoldevst.instrument.multiout",
    "YeOldeVST Instrument (multi-out)",
    "Elrinth",
    "https://github.com/Elrinth/YeOldeVST",
    "",
    "",
    kVersion,
    "Hosts old VST 1.x/2.x instruments with up to 16 stereo outputs (32-bit and 64-bit)",
    kInstrumentFeatures,
};

const clap_plugin_descriptor_t* Plugin::descriptor(Flavour flavour) {
  switch (flavour) {
    case Flavour::Effect: return &kEffectDesc;
    case Flavour::InstrumentMulti: return &kInstrumentMultiDesc;
    default: return &kInstrumentDesc;
  }
}

// ---------------------------------------------------------------------------------------------
// C callback glue

struct PluginGlue {
  static Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

  static bool init(const clap_plugin_t* p) { return self(p)->init(); }
  static void destroy(const clap_plugin_t* p) { self(p)->destroy(); }
  static bool activate(const clap_plugin_t* p, double sr, uint32_t mn, uint32_t mx) {
    return self(p)->activate(sr, mn, mx);
  }
  static void deactivate(const clap_plugin_t* p) { self(p)->deactivate(); }
  static bool startProcessing(const clap_plugin_t* p) { return self(p)->startProcessing(); }
  static void stopProcessing(const clap_plugin_t* p) { self(p)->stopProcessing(); }
  static void reset(const clap_plugin_t* p) { self(p)->reset(); }
  static clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) { return self(p)->process(pr); }
  static const void* getExtension(const clap_plugin_t* p, const char* id) { return self(p)->getExtension(id); }
  static void onMainThread(const clap_plugin_t* p) { self(p)->onMainThread(); }

  // audio ports
  static uint32_t audioPortsCount(const clap_plugin_t* p, bool isInput) {
    return isInput ? (self(p)->instrument_ ? 0 : 1) : self(p)->outPairs_;
  }
  static bool audioPortsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (isInput && (index != 0 || self(p)->instrument_)) return false;
    if (!isInput && index >= self(p)->outPairs_) return false;
    info->id = index;
    if (isInput)
      snprintf(info->name, sizeof info->name, "Input");
    else if (self(p)->outPairs_ == 1)
      snprintf(info->name, sizeof info->name, "Output");
    else
      snprintf(info->name, sizeof info->name, "Out %u/%u", index * 2 + 1, index * 2 + 2);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
  }
  static constexpr clap_plugin_audio_ports_t audioPorts{audioPortsCount, audioPortsGet};

  // note ports
  // One MIDI input, and one MIDI output for what the hosted plugin sends (arpeggiators etc.).
  static uint32_t notePortsCount(const clap_plugin_t*, bool) { return 1; }
  static bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
    if (index != 0) return false;
    info->id = 0;
    if (isInput) {
      info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
      info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
      snprintf(info->name, sizeof info->name, "MIDI In");
    } else {
      info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
      info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
      snprintf(info->name, sizeof info->name, "MIDI Out");
    }
    return true;
  }
  static constexpr clap_plugin_note_ports_t notePorts{notePortsCount, notePortsGet};

  // params
  static uint32_t paramsCount(const clap_plugin_t* p) { return self(p)->paramsCount(); }
  static bool paramsInfo(const clap_plugin_t* p, uint32_t i, clap_param_info_t* info) {
    return self(p)->paramsInfo(i, info);
  }
  static bool paramsValue(const clap_plugin_t* p, clap_id id, double* v) { return self(p)->paramsValue(id, v); }
  static bool paramsValueToText(const clap_plugin_t* p, clap_id id, double v, char* out, uint32_t size) {
    return self(p)->paramsValueToText(id, v, out, size);
  }
  static bool paramsTextToValue(const clap_plugin_t* p, clap_id id, const char* t, double* v) {
    return self(p)->paramsTextToValue(id, t, v);
  }
  static void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
    self(p)->paramsFlush(in, out);
  }
  static constexpr clap_plugin_params_t params{paramsCount, paramsInfo, paramsValue, paramsValueToText,
                                               paramsTextToValue, paramsFlush};

  // state
  static bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) { return self(p)->stateSave(s); }
  static bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) { return self(p)->stateLoad(s); }
  static constexpr clap_plugin_state_t state{stateSave, stateLoad};

  // latency
  static uint32_t latencyGet(const clap_plugin_t* p) { return self(p)->latency(); }
  static constexpr clap_plugin_latency_t latency{latencyGet};

  // tail
  static uint32_t tailGet(const clap_plugin_t* p) {
    auto h = self(p)->hosted();
    return h ? (uint32_t)std::max(0, h->info().tailSamples) : 0;
  }
  static constexpr clap_plugin_tail_t tail{tailGet};

  // gui
  static bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && std::strcmp(api, Gui::windowApi()) == 0;
  }
  static bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* floating) {
    *api = Gui::windowApi();
    *floating = false;
    return true;
  }
  static bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) {
    if (floating || std::strcmp(api, Gui::windowApi()) != 0) return false;
    auto s = self(p);
    if (!s->gui_) s->gui_ = Gui::create(*s);
    return s->gui_ != nullptr;
  }
  static void guiDestroy(const clap_plugin_t* p) { self(p)->gui_.reset(); }
  static bool guiSetScale(const clap_plugin_t*, double) { return false; }  // we follow the window's DPI
  static bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto s = self(p);
    if (!s->gui_) return false;
    s->gui_->getSize(*w, *h);
    return true;
  }
  static bool guiCanResize(const clap_plugin_t* p) { return true; }
  static bool guiGetResizeHints(const clap_plugin_t* p, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    hints->aspect_ratio_width = 1;
    hints->aspect_ratio_height = 1;
    return true;
  }
  static bool guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto s = self(p);
    if (!s->gui_) return false;
    s->gui_->adjustSize(*w, *h);
    return true;
  }
  static bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    auto s = self(p);
    return s->gui_ && s->gui_->setSize(w, h);
  }
  static bool guiSetParent(const clap_plugin_t* p, const clap_window_t* win) {
    auto s = self(p);
    return s->gui_ && s->gui_->setParent(win);
  }
  static bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
  static void guiSuggestTitle(const clap_plugin_t*, const char*) {}
  static bool guiShow(const clap_plugin_t* p) {
    auto s = self(p);
    if (s->gui_) s->gui_->show();
    return s->gui_ != nullptr;
  }
  static bool guiHide(const clap_plugin_t* p) {
    auto s = self(p);
    if (s->gui_) s->gui_->hide();
    return s->gui_ != nullptr;
  }
  static constexpr clap_plugin_gui_t gui{guiIsApiSupported, guiGetPreferredApi, guiCreate,      guiDestroy,
                                         guiSetScale,       guiGetSize,         guiCanResize,   guiGetResizeHints,
                                         guiAdjustSize,     guiSetSize,         guiSetParent,   guiSetTransient,
                                         guiSuggestTitle,   guiShow,            guiHide};

  // timer / fd support: how an X11 GUI gets called on the host's main thread
  static void onTimer(const clap_plugin_t* p, clap_id id) {
    if (auto& g = self(p)->gui_) g->onTimer(id);
  }
  static constexpr clap_plugin_timer_support_t timerSupport{onTimer};
  static void onFd(const clap_plugin_t* p, int fd, clap_posix_fd_flags_t flags) {
    if (auto& g = self(p)->gui_) g->onFd(fd, flags);
  }
  static constexpr clap_plugin_posix_fd_support_t fdSupport{onFd};
};

// ---------------------------------------------------------------------------------------------

Plugin::Plugin(const clap_host_t* host, Flavour flavour)
    : host_(host), instrument_(flavour != Flavour::Effect) {
  outPairs_ = flavour == Flavour::InstrumentMulti ? kMultiOutPairs : 1;
  plugin_.desc = descriptor(flavour);
  plugin_.plugin_data = this;
  plugin_.init = PluginGlue::init;
  plugin_.destroy = PluginGlue::destroy;
  plugin_.activate = PluginGlue::activate;
  plugin_.deactivate = PluginGlue::deactivate;
  plugin_.start_processing = PluginGlue::startProcessing;
  plugin_.stop_processing = PluginGlue::stopProcessing;
  plugin_.reset = PluginGlue::reset;
  plugin_.process = PluginGlue::process;
  plugin_.get_extension = PluginGlue::getExtension;
  plugin_.on_main_thread = PluginGlue::onMainThread;

  midiEvents_.resize(1024);
  midiOut_.reserve(2048);
  midiOutBlock_.reserve(2048);
  sysexEvents_.resize(64);
  sysexData_.resize(64);
  eventStore_.resize(sizeof(VstEvents) + sizeof(VstEvent*) * (midiEvents_.size() + sysexEvents_.size()));
  time_.sampleRate = sampleRate_;
  time_.tempo = 120;
  time_.timeSigNumerator = 4;
  time_.timeSigDenominator = 4;
  paramNames_.assign(0, {});
}

Plugin::~Plugin() = default;

bool Plugin::init() {
  mainThreadId_ = currentThreadId();
  hostParams_ = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
  hostLatency_ = static_cast<const clap_host_latency_t*>(host_->get_extension(host_, CLAP_EXT_LATENCY));
  hostState_ = static_cast<const clap_host_state_t*>(host_->get_extension(host_, CLAP_EXT_STATE));
  hostGui_ = static_cast<const clap_host_gui_t*>(host_->get_extension(host_, CLAP_EXT_GUI));
  hostThreadCheck_ =
      static_cast<const clap_host_thread_check_t*>(host_->get_extension(host_, CLAP_EXT_THREAD_CHECK));
  hostTimer_ = static_cast<const clap_host_timer_support_t*>(host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
  hostFd_ = static_cast<const clap_host_posix_fd_support_t*>(host_->get_extension(host_, CLAP_EXT_POSIX_FD_SUPPORT));
  wrappedAsVst3_ = host_->name && containsNoCase(host_->name, "clap-as-vst3");
  Shared::get();
  logLine("instance created (%s) in host '%s'", instrument_ ? "instrument" : "effect",
          host_->name ? host_->name : "?");
  // Testing aid: start new instances with a plugin already loaded, so host test tools that
  // can't pass a state still exercise a real VST2.
  std::string autoload;
  if (getEnv("YEOLDEVST_TEST_AUTOLOAD", autoload) && !autoload.empty()) {
    initializing_ = true;  // the host is not ready for rescan / restart requests yet
    PluginRecord rec;
    rec.path = autoload;
    rec.arch = archName(readModuleInfo(rec.path).arch);
    std::string err;
    if (!loadVst(rec, false, err)) logLine("autoload failed: %s", err.c_str());
    initializing_ = false;
  }
  return true;
}

void Plugin::destroy() {
  gui_.reset();
  if (slot_) {
    std::unique_ptr<Slot> old;
    {
      std::lock_guard<std::mutex> lk(procLock_);
      old = std::move(slot_);
    }
    if (old->plugin && !old->plugin->dead()) old->plugin->suspend();
  }
  delete this;
}

bool Plugin::isMainThread() const {
  if (hostThreadCheck_) return hostThreadCheck_->is_main_thread(host_);
  return currentThreadId() == mainThreadId_;
}

void Plugin::Slot::allocate(uint32_t frames) {
  int nin = std::max(1, plugin->info().numInputs);
  int nout = std::max(1, plugin->info().numOutputs);
  in.assign(nin, std::vector<float>(frames));
  out.assign(nout, std::vector<float>(frames));
  inPtr.clear();
  outPtr.clear();
  for (auto& b : in) inPtr.push_back(b.data());
  for (auto& b : out) outPtr.push_back(b.data());
}

bool Plugin::activate(double sr, uint32_t, uint32_t maxFrames) {
  sampleRate_ = sr;
  maxFrames_ = std::max<uint32_t>(maxFrames, 1);
  time_.sampleRate = sr;
  if (slot_ && slot_->plugin) {
    slot_->allocate(maxFrames_);
    slot_->plugin->setupProcessing(sr, (int32_t)maxFrames_);
    slot_->plugin->resume();
  }
  active_ = true;
  if (hostLatency_) hostLatency_->changed(host_);
  return true;
}

void Plugin::deactivate() {
  active_ = false;
  if (slot_ && slot_->plugin) slot_->plugin->suspend();
}

bool Plugin::startProcessing() { return true; }
void Plugin::stopProcessing() {}

void Plugin::reset() {
  samplePosCounter_ = 0;
}

const void* Plugin::getExtension(const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &PluginGlue::audioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &PluginGlue::notePorts;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &PluginGlue::params;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &PluginGlue::state;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &PluginGlue::latency;
  if (!std::strcmp(id, CLAP_EXT_TAIL)) return &PluginGlue::tail;
  if (!std::strcmp(id, CLAP_EXT_GUI)) return &PluginGlue::gui;
#if !defined(_WIN32) && !defined(__APPLE__)
  if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &PluginGlue::timerSupport;
  if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &PluginGlue::fdSupport;
#endif
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Loading

void Plugin::installSlot(std::unique_ptr<Slot> s) {
  std::unique_ptr<Slot> old;
  {
    std::lock_guard<std::mutex> lk(procLock_);
    old = std::move(slot_);
    slot_ = std::move(s);
  }
  if (old && old->plugin) {
    if (gui_) gui_->beforeUnload(old->plugin.get());
    if (!old->plugin->dead()) old->plugin->suspend();
    old.reset();  // effClose, FreeLibrary (or ends the bridge process)
  }
}

bool Plugin::loadVst(const PluginRecord& rec, bool sandboxed, std::string& error) {
  bool is32 = rec.arch == "x86";
  std::unique_ptr<HostedPlugin> hp;
  if (is32 || sandboxed)
    hp = loadBridged(rec.path, rec.shellId, is32, this, error);
  else
    hp = loadInProcess(rec.path, rec.shellId, this, error);
  if (!hp) {
    logLine("load failed for %s: %s", rec.path.c_str(), error.c_str());
    return false;
  }
  auto slot = std::make_unique<Slot>();
  slot->plugin = std::move(hp);
  slot->allocate(maxFrames_);
  slot->plugin->setupProcessing(sampleRate_, (int32_t)maxFrames_);
  if (active_) slot->plugin->resume();

  // Close the old editor before swapping so the GUI never points at a dead plugin.
  if (gui_ && slot_) gui_->beforeUnload(slot_->plugin.get());
  installSlot(std::move(slot));

  loaded_ = rec;
  loadedSandboxed_ = is32 || sandboxed;
  if (loaded_.name.empty()) loaded_.name = hosted()->info().name;
  missingState_.clear();
  missingText_.clear();
  crashReported_ = false;
  genericView_ = !hosted()->info().hasEditor;
  rebuildParamNames();
  lastGoodState_ = buildStateJson();
  if (!initializing_) {
    notifyParamsChanged(true);
    if (active_) host_->request_restart(host_);  // latency may have changed
    if (hostState_) hostState_->mark_dirty(host_);
  }
  if (gui_) gui_->onPluginChanged();
  logLine("loaded %s (%s)", rec.path.c_str(), loadedSandboxed_ ? "bridged" : "in-process");
  return true;
}

void Plugin::unloadVst() {
  if (gui_ && slot_) gui_->beforeUnload(slot_->plugin.get());
  installSlot(nullptr);
  loaded_ = PluginRecord{};
  paramNames_.clear();
  notifyParamsChanged(true);
  if (active_) host_->request_restart(host_);
  if (hostState_) hostState_->mark_dirty(host_);
  if (gui_) gui_->onPluginChanged();
}

bool Plugin::reload(std::string& error) {
  std::string state = lastGoodState_;
  if (state.empty()) state = buildStateJson();
  // A dead in-process plugin must not be closed (that would call into it); the slot destructor
  // handles that by leaking it.
  return applyStateJson(state, error);
}

void Plugin::setProgram(int32_t index) {
  if (auto h = hosted()) {
    h->setProgram(index);
    notifyParamsChanged(false);
    if (hostState_) hostState_->mark_dirty(host_);
  }
}

void Plugin::rebuildParamNames() {
  paramNames_.clear();
  auto h = hosted();
  if (!h) return;
  int n = std::min<int>(h->info().numParams, (int)kMaxParams);
  paramNames_ = h->paramNames(n);
  paramNames_.resize(n);
  for (int i = 0; i < n; ++i)
    if (paramNames_[i].empty()) paramNames_[i] = "Param " + std::to_string(i + 1);
}

void Plugin::notifyParamsChanged(bool full) {
  if (!hostParams_) return;
  if (full && wrappedAsVst3_) {
    // VST3 hosts only re-read titles when the wrapper rebuilds its parameter list.
    hostParams_->rescan(host_, CLAP_PARAM_RESCAN_ALL);
    return;
  }
  hostParams_->rescan(host_, full ? (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT)
                                  : (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT));
}

void Plugin::requestResize(uint32_t w, uint32_t h) {
  if (hostGui_) hostGui_->request_resize(host_, w, h);
}

// ---------------------------------------------------------------------------------------------
// Listener (VST2 -> us)

void Plugin::queueOut(OutEvent e) {
  std::lock_guard<std::mutex> lk(outLock_);
  if (outQueue_.size() < 4096) outQueue_.push_back(e);
}

void Plugin::hostSetParameter(HostedPlugin* h, uint32_t index, double value) {
  float v = (float)std::clamp(value, 0.0, 1.0);
  if (index < kMaxParams) {
    hostSet_[index].value.store(v, std::memory_order_relaxed);
    hostSet_[index].ms.store((uint32_t)nowMs(), std::memory_order_relaxed);
  }
  h->setParameter((int32_t)index, v);
}

void Plugin::vstAutomate(int32_t index, float value) {
  if (index < 0 || (uint32_t)index >= kMaxParams) return;
  // An echo of a value the host itself just set (some plugins report it from their UI timer,
  // after the call): passing it on would look like the user turned the knob.
  auto& hs = hostSet_[index];
  if (std::fabs(hs.value.load(std::memory_order_relaxed) - value) < 1e-6f &&
      (uint32_t)nowMs() - hs.ms.load(std::memory_order_relaxed) < 500)
    return;
  queueOut({OutEvent::Value, (uint32_t)index, (double)value});
  if (hostParams_) hostParams_->request_flush(host_);
  if (gui_ && isMainThread()) gui_->onParamValue((uint32_t)index);
}

void Plugin::vstBeginEdit(int32_t index) {
  if (index < 0 || (uint32_t)index >= kMaxParams) return;
  queueOut({OutEvent::Begin, (uint32_t)index, 0});
  if (hostParams_) hostParams_->request_flush(host_);
}

void Plugin::vstEndEdit(int32_t index) {
  if (index < 0 || (uint32_t)index >= kMaxParams) return;
  queueOut({OutEvent::End, (uint32_t)index, 0});
  if (hostParams_) hostParams_->request_flush(host_);
}

bool Plugin::vstSizeWindow(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
  if (isMainThread()) {
    if (gui_) gui_->onEditorResize(width, height);
  } else {
    pendingSize_.store(((uint32_t)width << 16) | (uint32_t)height);
    host_->request_callback(host_);
  }
  return true;
}

void Plugin::vstIOChanged() {
  pendingIO_ = true;
  host_->request_callback(host_);
}

void Plugin::vstUpdateDisplay() {
  pendingDisplay_ = true;
  host_->request_callback(host_);
}

void Plugin::onMainThread() {
  if (pendingCrash_.exchange(false)) {
    if (gui_) gui_->onCrash();
  }
  if (uint32_t sz = pendingSize_.exchange(0)) {
    if (gui_) gui_->onEditorResize((int)(sz >> 16), (int)(sz & 0xFFFF));
  }
  if (pendingIO_.exchange(false)) {
    if (auto h = hosted()) {
      int oldDelay = h->info().initialDelay;
      int oldIn = h->info().numInputs, oldOut = h->info().numOutputs;
      h->refreshInfo();
      if (h->info().numInputs != oldIn || h->info().numOutputs != oldOut) {
        // Channel count changed: reallocate buffers under the lock.
        std::lock_guard<std::mutex> lk(procLock_);
        slot_->allocate(maxFrames_);
      }
      if (h->info().initialDelay != oldDelay && active_) host_->request_restart(host_);
    }
  }
  if (pendingDisplay_.exchange(false)) {
    rebuildParamNames();
    notifyParamsChanged(false);
    if (gui_) gui_->onParamsChanged();
  }
}

// ---------------------------------------------------------------------------------------------
// Audio

void Plugin::updateTime(const clap_event_transport_t* t, uint32_t frames) {
  VstTimeInfo& ti = time_;
  ti.sampleRate = sampleRate_;
  int32_t flags = kVstNanosValid;
  ti.nanoSeconds = (double)nowMs() * 1e6;
  if (t) {
    bool playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    if (playing) flags |= kVstTransportPlaying;
    if (t->flags & CLAP_TRANSPORT_IS_RECORDING) flags |= kVstTransportRecording;
    if (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) flags |= kVstTransportCycleActive;
    if (playing != wasPlaying_) flags |= kVstTransportChanged;
    wasPlaying_ = playing;
    if (t->flags & CLAP_TRANSPORT_HAS_TEMPO) {
      ti.tempo = t->tempo;
      flags |= kVstTempoValid;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
      ti.ppqPos = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
      ti.barStartPos = (double)t->bar_start / (double)CLAP_BEATTIME_FACTOR;
      ti.cycleStartPos = (double)t->loop_start_beats / (double)CLAP_BEATTIME_FACTOR;
      ti.cycleEndPos = (double)t->loop_end_beats / (double)CLAP_BEATTIME_FACTOR;
      flags |= kVstPpqPosValid | kVstBarsValid | kVstCyclePosValid;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
      ti.timeSigNumerator = t->tsig_num;
      ti.timeSigDenominator = t->tsig_denom;
      flags |= kVstTimeSigValid;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) {
      ti.samplePos = (double)t->song_pos_seconds / (double)CLAP_SECTIME_FACTOR * sampleRate_;
    } else if ((t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && ti.tempo > 0) {
      ti.samplePos = ti.ppqPos * 60.0 / ti.tempo * sampleRate_;
    } else {
      ti.samplePos = samplePosCounter_;
    }
  } else {
    ti.samplePos = samplePosCounter_;
    flags |= kVstTempoValid | kVstTimeSigValid;
  }
  if (ti.samplePos < 0) ti.samplePos = 0;
  ti.flags = flags;
  samplePosCounter_ += frames;
}

void Plugin::handleInputEvents(const clap_input_events_t* in, Slot* slot, uint32_t frames) {
  auto* evs = reinterpret_cast<VstEvents*>(eventStore_.data());
  evs->numEvents = 0;
  evs->reserved = 0;
  size_t nMidi = 0, nSysex = 0;
  auto h = slot ? slot->plugin.get() : nullptr;
  uint32_t n = in ? in->size(in) : 0;
  auto addMidi = [&](uint32_t time, uint8_t b0, uint8_t b1, uint8_t b2) {
    if (nMidi >= midiEvents_.size()) return;
    VstMidiEvent& m = midiEvents_[nMidi++];
    std::memset(&m, 0, sizeof m);
    m.type = kVstMidiType;
    m.byteSize = sizeof(VstMidiEvent);
    m.deltaFrames = (int32_t)std::min(time, frames ? frames - 1 : 0);
    m.flags = kVstMidiEventIsRealtime;
    m.midiData[0] = (char)b0;
    m.midiData[1] = (char)b1;
    m.midiData[2] = (char)b2;
    evs->events[evs->numEvents++] = reinterpret_cast<VstEvent*>(&m);
  };
  for (uint32_t i = 0; i < n; ++i) {
    auto hdr = in->get(in, i);
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
    switch (hdr->type) {
      case CLAP_EVENT_PARAM_VALUE: {
        auto ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
        if (h && ev->param_id < (clap_id)h->info().numParams)
          hostSetParameter(h, ev->param_id, ev->value);
        break;
      }
      case CLAP_EVENT_NOTE_ON: {
        auto ev = reinterpret_cast<const clap_event_note_t*>(hdr);
        if (ev->key < 0 || ev->key > 127) break;
        int vel = std::clamp((int)std::lround(ev->velocity * 127.0), 1, 127);
        addMidi(hdr->time, uint8_t(0x90 | (std::max<int16_t>(ev->channel, 0) & 15)), (uint8_t)ev->key, (uint8_t)vel);
        break;
      }
      case CLAP_EVENT_NOTE_OFF:
      case CLAP_EVENT_NOTE_CHOKE: {
        auto ev = reinterpret_cast<const clap_event_note_t*>(hdr);
        if (ev->key < 0 || ev->key > 127) break;
        int vel = std::clamp((int)std::lround(ev->velocity * 127.0), 0, 127);
        addMidi(hdr->time, uint8_t(0x80 | (std::max<int16_t>(ev->channel, 0) & 15)), (uint8_t)ev->key, (uint8_t)vel);
        break;
      }
      case CLAP_EVENT_MIDI: {
        auto ev = reinterpret_cast<const clap_event_midi_t*>(hdr);
        addMidi(hdr->time, ev->data[0], ev->data[1], ev->data[2]);
        break;
      }
      case CLAP_EVENT_MIDI_SYSEX: {
        auto ev = reinterpret_cast<const clap_event_midi_sysex_t*>(hdr);
        if (nSysex >= sysexEvents_.size() || !ev->buffer || ev->size == 0) break;
        auto& data = sysexData_[nSysex];
        data.assign((const char*)ev->buffer, (const char*)ev->buffer + ev->size);
        VstMidiSysexEvent& s = sysexEvents_[nSysex++];
        std::memset(&s, 0, sizeof s);
        s.type = kVstSysExType;
        s.byteSize = sizeof(VstMidiSysexEvent);
        s.deltaFrames = (int32_t)std::min(hdr->time, frames ? frames - 1 : 0);
        s.dumpBytes = (int32_t)data.size();
        s.sysexDump = data.data();
        evs->events[evs->numEvents++] = reinterpret_cast<VstEvent*>(&s);
        break;
      }
      case CLAP_EVENT_TRANSPORT: break;  // handled via process->transport
      default: break;
    }
  }
}

void Plugin::emitOutputEvents(const clap_output_events_t* out) {
  if (!out) return;
  std::unique_lock<std::mutex> lk(outLock_, std::try_to_lock);
  if (!lk.owns_lock() || outQueue_.empty()) return;
  for (auto& e : outQueue_) {
    if (e.kind == OutEvent::Value) {
      clap_event_param_value_t ev{};
      ev.header.size = sizeof ev;
      ev.header.time = 0;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      ev.header.type = CLAP_EVENT_PARAM_VALUE;
      ev.header.flags = 0;
      ev.param_id = e.index;
      ev.cookie = nullptr;
      ev.note_id = -1;
      ev.port_index = -1;
      ev.channel = -1;
      ev.key = -1;
      ev.value = e.value;
      out->try_push(out, &ev.header);
    } else {
      clap_event_param_gesture_t ev{};
      ev.header.size = sizeof ev;
      ev.header.time = 0;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      ev.header.type = e.kind == OutEvent::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
      ev.header.flags = 0;
      ev.param_id = e.index;
      out->try_push(out, &ev.header);
    }
  }
  outQueue_.clear();
}

clap_process_status Plugin::process(const clap_process_t* p) {
  const uint32_t frames = p->frames_count;
  float* outL = nullptr;
  float* outR = nullptr;
  if (p->audio_outputs_count > 0 && p->audio_outputs[0].channel_count > 0) {
    outL = p->audio_outputs[0].data32[0];
    outR = p->audio_outputs[0].channel_count > 1 ? p->audio_outputs[0].data32[1] : nullptr;
  }
  const float* inL = nullptr;
  const float* inR = nullptr;
  if (!instrument_ && p->audio_inputs_count > 0 && p->audio_inputs[0].channel_count > 0) {
    inL = p->audio_inputs[0].data32[0];
    inR = p->audio_inputs[0].channel_count > 1 ? p->audio_inputs[0].data32[1] : inL;
  }
  auto passThroughOrSilence = [&] {
    if (outL) {
      if (inL) std::memmove(outL, inL, frames * sizeof(float));
      else std::memset(outL, 0, frames * sizeof(float));
    }
    if (outR) {
      if (inR) std::memmove(outR, inR, frames * sizeof(float));
      else std::memset(outR, 0, frames * sizeof(float));
    }
  };

  // Extra stereo pairs (multi-out flavour) start silent; filled below from the plugin's
  // outputs 3/4, 5/6, ...
  for (uint32_t port = 1; port < p->audio_outputs_count; ++port)
    for (uint32_t ch = 0; ch < p->audio_outputs[port].channel_count; ++ch)
      std::memset(p->audio_outputs[port].data32[ch], 0, frames * sizeof(float));

  std::unique_lock<std::mutex> lk(procLock_, std::try_to_lock);
  if (!lk.owns_lock()) {
    // A plugin is being swapped on the main thread right now.
    passThroughOrSilence();
    return CLAP_PROCESS_CONTINUE;
  }
  Slot* slot = slot_.get();
  handleInputEvents(p->in_events, slot, frames);
  updateTime(p->transport, frames);

  bool ok = false;
  if (slot && slot->plugin && frames > 0 && frames <= maxFrames_ && !slot->plugin->dead()) {
    auto& info = slot->plugin->info();
    // Map our stereo input onto the plugin's inputs.
    for (size_t c = 0; c < slot->in.size(); ++c) {
      float* dst = slot->in[c].data();
      if (!inL || (int)c >= std::max(1, info.numInputs)) {
        std::memset(dst, 0, frames * sizeof(float));
      } else if (info.numInputs == 1) {
        for (uint32_t i = 0; i < frames; ++i) dst[i] = 0.5f * (inL[i] + inR[i]);
      } else if (c == 0) {
        std::memcpy(dst, inL, frames * sizeof(float));
      } else if (c == 1) {
        std::memcpy(dst, inR, frames * sizeof(float));
      } else {
        std::memset(dst, 0, frames * sizeof(float));
      }
    }
    auto* evs = reinterpret_cast<const VstEvents*>(eventStore_.data());
    ok = slot->plugin->process(slot->inPtr.data(), slot->outPtr.data(), (int32_t)frames, evs, time_);
    if (ok && info.numOutputs > 0) {
      if (outL) std::memcpy(outL, slot->out[0].data(), frames * sizeof(float));
      if (outR) std::memcpy(outR, slot->out[info.numOutputs > 1 ? 1 : 0].data(), frames * sizeof(float));
      for (uint32_t port = 1; port < p->audio_outputs_count; ++port)
        for (uint32_t ch = 0; ch < p->audio_outputs[port].channel_count && ch < 2; ++ch) {
          int src = (int)(port * 2 + ch);
          if (src < info.numOutputs && src < (int)slot->out.size())
            std::memcpy(p->audio_outputs[port].data32[ch], slot->out[src].data(), frames * sizeof(float));
        }
    } else if (ok) {
      passThroughOrSilence();
    }
    if (slot->plugin->dead() && !crashReported_) {
      crashReported_ = true;
      pendingCrash_ = true;
      host_->request_callback(host_);
    }
  }
  if (!ok) {
    if (slot && slot->plugin && !slot->plugin->dead()) {
      // Plugin is fine but did not deliver (bridge busy): keep the gap silent.
      if (outL) std::memset(outL, 0, frames * sizeof(float));
      if (outR) std::memset(outR, 0, frames * sizeof(float));
    } else if (slot && slot->plugin) {
      // Crashed: mute (passing the dry signal through would be a surprise on an instrument).
      if (outL) std::memset(outL, 0, frames * sizeof(float));
      if (outR) std::memset(outR, 0, frames * sizeof(float));
    } else {
      passThroughOrSilence();
    }
  }
  lk.unlock();
  emitOutputEvents(p->out_events);
  emitMidiOut(p->out_events, frames);
  return CLAP_PROCESS_CONTINUE;
}

void Plugin::vstMidiOut(const VstEvents* events) {
  if (!events) return;
  std::lock_guard<std::mutex> lk(midiOutLock_);
  for (int32_t i = 0; i < events->numEvents && midiOut_.size() < 2048; ++i) {
    const VstEvent* e = events->events[i];
    if (!e) continue;
    if (e->type == kVstMidiType) {
      auto m = reinterpret_cast<const VstMidiEvent*>(e);
      MidiOut o{(uint32_t)std::max(0, m->deltaFrames), {(uint8_t)m->midiData[0], (uint8_t)m->midiData[1],
                                                         (uint8_t)m->midiData[2]}, -1};
      if (o.bytes[0] & 0x80) midiOut_.push_back(o);
    } else if (e->type == kVstSysExType && midiOutSysex_.size() < 64) {
      auto x = reinterpret_cast<const VstMidiSysexEvent*>(e);
      if (!x->sysexDump || x->dumpBytes <= 0 || x->dumpBytes > (1 << 20)) continue;
      midiOutSysex_.emplace_back((const uint8_t*)x->sysexDump, (const uint8_t*)x->sysexDump + x->dumpBytes);
      midiOut_.push_back({(uint32_t)std::max(0, x->deltaFrames), {0, 0, 0}, (int32_t)midiOutSysex_.size() - 1});
    }
  }
}

void Plugin::emitMidiOut(const clap_output_events_t* out, uint32_t frames) {
  if (!out) return;
  {
    std::unique_lock<std::mutex> lk(midiOutLock_, std::try_to_lock);
    if (!lk.owns_lock()) return;  // the plugin is sending right now: next block
    midiOutBlock_.clear();
    midiOutSysexBlock_.clear();
    midiOut_.swap(midiOutBlock_);
    midiOutSysex_.swap(midiOutSysexBlock_);
  }
  if (midiOutBlock_.empty()) return;
  // Output events must be in time order.
  std::stable_sort(midiOutBlock_.begin(), midiOutBlock_.end(),
                   [](const MidiOut& a, const MidiOut& b) { return a.time < b.time; });
  uint32_t last = frames ? frames - 1 : 0;
  for (auto& m : midiOutBlock_) {
    uint32_t t = std::min(m.time, last);
    if (m.sysex < 0) {
      clap_event_midi_t ev{};
      ev.header = {sizeof ev, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
      ev.port_index = 0;
      memcpy(ev.data, m.bytes, 3);
      out->try_push(out, &ev.header);
    } else {
      auto& d = midiOutSysexBlock_[(size_t)m.sysex];  // stays alive until the next block
      clap_event_midi_sysex_t ev{};
      ev.header = {sizeof ev, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
      ev.port_index = 0;
      ev.buffer = d.data();
      ev.size = (uint32_t)d.size();
      out->try_push(out, &ev.header);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Params

uint32_t Plugin::paramsCount() { return kMaxParams; }

bool Plugin::paramsInfo(uint32_t index, clap_param_info_t* info) {
  if (index >= kMaxParams) return false;
  std::memset(info, 0, sizeof *info);
  info->id = index;
  info->min_value = 0;
  info->max_value = 1;
  info->default_value = 0;
  info->cookie = nullptr;
  if (index < paramNames_.size()) {
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof info->name, "%s", paramNames_[index].c_str());
  } else if (paramNames_.empty() && index < kGenericParams) {
    // No plugin yet. Many hosts read the parameter list only once, right after creating us, and
    // ignore later rescans: give them generic slots that map 1:1 to the plugin's parameters once
    // one is loaded. Hosts that do rescan get the exact list.
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof info->name, "Param %u", index + 1);
  } else {
    info->flags = CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY;
    snprintf(info->name, sizeof info->name, "(unused %u)", index + 1);
  }
  snprintf(info->module, sizeof info->module, "%s", "");
  return true;
}

bool Plugin::paramsValue(clap_id id, double* value) {
  if (id >= kMaxParams) return false;
  auto h = hosted();
  *value = (h && id < paramNames_.size()) ? std::clamp((double)h->getParameter((int32_t)id), 0.0, 1.0) : 0.0;
  return true;
}

bool Plugin::paramsValueToText(clap_id id, double value, char* out, uint32_t size) {
  if (id >= kMaxParams || size == 0) return false;
  auto h = hosted();
  if (h && id < paramNames_.size() && std::fabs(h->getParameter((int32_t)id) - value) < 1e-5) {
    auto s = h->paramDisplay((int32_t)id);
    if (!s.empty()) {
      snprintf(out, size, "%s", s.c_str());
      return true;
    }
  }
  snprintf(out, size, "%.1f%%", value * 100.0);
  return true;
}

bool Plugin::paramsTextToValue(clap_id id, const char* text, double* value) {
  if (id >= kMaxParams || !text) return false;
  char* end = nullptr;
  double v = std::strtod(text, &end);
  if (end == text) return false;
  while (*end == ' ') ++end;
  if (*end == '%' || v > 1.0) v /= 100.0;
  *value = std::clamp(v, 0.0, 1.0);
  return true;
}

void Plugin::paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out) {
  // Not processing: apply parameter changes directly (no audio buffers involved).
  if (in) {
    auto h = hosted();
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
      auto hdr = in->get(in, i);
      if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE && h) {
        auto ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
        if (ev->param_id < (clap_id)h->info().numParams)
          hostSetParameter(h, ev->param_id, ev->value);
      }
    }
  }
  emitOutputEvents(out);
}

void Plugin::editParameter(uint32_t index, double value) {
  auto h = hosted();
  if (!h || index >= paramNames_.size()) return;
  h->setParameter((int32_t)index, (float)value);
  queueOut({OutEvent::Value, index, value});
  if (hostParams_) hostParams_->request_flush(host_);
}

void Plugin::beginParameterEdit(uint32_t index) { vstBeginEdit((int32_t)index); }
void Plugin::endParameterEdit(uint32_t index) { vstEndEdit((int32_t)index); }

uint32_t Plugin::latency() {
  auto h = hosted();
  return h ? (uint32_t)std::max(0, h->info().initialDelay) : 0;
}

// ---------------------------------------------------------------------------------------------
// State

std::string Plugin::buildStateJson() {
  if (!hosted()) return missingState_;
  auto h = hosted();
  Json j = Json::object();
  j["format"] = "YeOldeVST";
  j["version"] = 1;
  j["path"] = loaded_.path;
  j["shellId"] = (int64_t)loaded_.shellId;
  j["uniqueId"] = (int64_t)h->info().uniqueId;
  j["name"] = loaded_.name;
  j["arch"] = loaded_.arch;
  j["sandboxed"] = loadedSandboxed_;
  j["genericView"] = genericView_;
  if (h->dead()) {
    // Can't ask a crashed plugin for anything: keep what we had.
    return lastGoodState_;
  }
  j["program"] = (int64_t)h->currentProgram();
  std::vector<uint8_t> chunk;
  bool isPreset = true;
  if (h->info().usesChunks && h->getChunk(chunk, isPreset)) {
    j["chunk"] = base64Encode(chunk.data(), chunk.size());
    j["chunkType"] = isPreset ? "program" : "bank";
  } else {
    Json params = Json::array();
    for (int i = 0; i < h->info().numParams && i < 100000; ++i) params.push((double)h->getParameter(i));
    j["params"] = std::move(params);
  }
  return j.dump();
}

bool Plugin::applyStateJson(const std::string& text, std::string& error) {
  Json j;
  if (!Json::parse(text, j) || j.get("format").asString() != "YeOldeVST") {
    error = "not a YeOldeVST state";
    return false;
  }
  PluginRecord rec;
  rec.path = j.get("path").asString();
  rec.shellId = (int32_t)j.get("shellId").asInt();
  rec.name = j.get("name").asString();
  rec.arch = j.get("arch").asString();
  rec.uniqueId = (int32_t)j.get("uniqueId").asInt();
  bool sandboxed = j.get("sandboxed").asBool();

  auto& db = Shared::get().db;
  db.reloadIfChanged();
  PluginRecord found;
  if (db.find(rec.path, rec.shellId, found)) rec = found;
  if (!fileExists(rec.path)) {
    if (db.findMoved(rec.uniqueId, rec.name, found)) {
      logLine("plugin moved: %s -> %s", rec.path.c_str(), found.path.c_str());
      rec = found;
    }
  }
  if (rec.arch.empty()) rec.arch = archName(readModuleInfo(rec.path).arch);
  if (!fileExists(rec.path)) {
    if (gui_ && slot_) gui_->beforeUnload(slot_->plugin.get());
    installSlot(nullptr);
    loaded_ = PluginRecord{};
    paramNames_.clear();
    missingState_ = text;
    missingText_ = "Missing plugin: " + (rec.name.empty() ? fileNameOf(rec.path) : rec.name) + " (" + rec.path +
                   "). Its settings are kept; add the folder it now lives in and scan, then reopen the project.";
    error = missingText_;
    notifyParamsChanged(true);
    if (gui_) gui_->onPluginChanged();
    return false;
  }
  if (!loadVst(rec, sandboxed, error)) {
    missingState_ = text;
    missingText_ = "Could not load " + rec.name + ": " + error;
    if (gui_) gui_->onPluginChanged();
    return false;
  }
  auto h = hosted();
  genericView_ = j.get("genericView").asBool(!h->info().hasEditor);
  if (j.has("program")) h->setProgram((int32_t)j.get("program").asInt());
  if (j.has("chunk")) {
    std::vector<uint8_t> data;
    if (base64Decode(j.get("chunk").asString(), data) && !data.empty())
      h->setChunk(data, j.get("chunkType").asString() != "bank");
  } else {
    auto& params = j.get("params").elements();
    for (size_t i = 0; i < params.size() && (int)i < h->info().numParams; ++i)
      h->setParameter((int32_t)i, (float)params[i].asNumber());
  }
  rebuildParamNames();
  notifyParamsChanged(false);
  lastGoodState_ = text;
  if (gui_) gui_->onPluginChanged();
  return true;
}

bool Plugin::stateSave(const clap_ostream_t* stream) {
  std::string s = buildStateJson();
  if (hosted() && !hosted()->dead()) lastGoodState_ = s;
  const char* data = s.data();
  int64_t left = (int64_t)s.size();
  while (left > 0) {
    int64_t w = stream->write(stream, data, (uint64_t)left);
    if (w <= 0) return false;
    data += w;
    left -= w;
  }
  return true;
}

bool Plugin::stateLoad(const clap_istream_t* stream) {
  std::string s;
  char buf[65536];
  for (;;) {
    int64_t r = stream->read(stream, buf, sizeof buf);
    if (r < 0) return false;
    if (r == 0) break;
    s.append(buf, (size_t)r);
  }
  if (s.empty()) {
    unloadVst();
    return true;
  }
  std::string err;
  applyStateJson(s, err);
  // A missing plugin is not a failure for the host: we keep the state and show why.
  return true;
}

}  // namespace yov
