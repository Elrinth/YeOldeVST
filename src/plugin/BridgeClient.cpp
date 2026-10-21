// Client side of the bridge: a HostedPlugin whose VST2 runs in a helper process.
// 32-bit plugins always go this way; 64-bit ones when loaded "sandboxed".
#include "common/BridgeProtocol.h"
#include "common/PeInfo.h"
#include "common/Seh.h"
#include "common/Util.h"
#include "plugin/HostedPlugin.h"
#include "scan/Scanner.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

using namespace yov::bridge;

namespace yov {

namespace {

HANDLE bridgeJob() {
  static HANDLE job = [] {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (j) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
      SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return j;
  }();
  return job;
}

class BridgedPlugin final : public HostedPlugin {
 public:
  BridgedPlugin(Listener* l) : listener_(l) { mainThread_ = GetCurrentThreadId(); }
  ~BridgedPlugin() override { shutdown(); }

  bool start(const std::string& path, int32_t shellId, bool is32, std::string& error);

  bool sandboxed() const override { return true; }
  const HostedInfo& info() const override { return info_; }
  void refreshInfo() override {
    if (call(OpRefreshInfo)) copyInfo();
  }

  bool dead() const override {
    if (broken_.load()) return true;
    if (proc_ && WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) return true;
    return sh_ && sh_->dead;
  }
  std::string deathReason() const override {
    if (sh_ && sh_->dead) return std::string(sh_->deathReason, strnlen(sh_->deathReason, sizeof sh_->deathReason));
    DWORD code = 0;
    if (proc_ && WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(proc_, &code)) {
      char b[160];
      snprintf(b, sizeof b, "the plugin's process ended: %s (0x%08lX)", exceptionName(code), code);
      return b;
    }
    if (broken_.load()) return "the plugin stopped responding and was shut down";
    return {};
  }

  void setupProcessing(double sr, int32_t maxFrames) override {
    sr_ = sr;
    call(OpSetup, std::min<int32_t>(maxFrames, (int32_t)kMaxFrames), 0, sr);
  }
  void resume() override { call(OpResume); }
  void suspend() override { call(OpSuspend); }

  std::string paramName(int32_t i) override { return callString(OpParamName, i); }
  std::vector<std::string> paramNames(int32_t count) override {
    std::vector<uint8_t> out;
    std::vector<std::string> names;
    if (!call(OpParamNames, count, 0, 0, nullptr, 0, &out)) return names;
    size_t start = 0;
    for (size_t i = 0; i < out.size(); ++i)
      if (out[i] == 0) {
        names.emplace_back((const char*)out.data() + start, i - start);
        start = i + 1;
      }
    return names;
  }
  std::string paramDisplay(int32_t i) override { return callString(OpParamDisplay, i); }
  int32_t currentProgram() override { return call(OpGetProgram) ? lastResult_ : 0; }
  void setProgram(int32_t i) override { call(OpSetProgram, i); }
  std::vector<std::string> programNames() override {
    std::vector<uint8_t> out;
    std::vector<std::string> names;
    if (!call(OpProgramNames, 0, 0, 0, nullptr, 0, &out)) return names;
    size_t start = 0;
    for (size_t i = 0; i < out.size(); ++i)
      if (out[i] == 0) {
        names.emplace_back((const char*)out.data() + start, i - start);
        start = i + 1;
      }
    return names;
  }
  bool getChunk(std::vector<uint8_t>& out, bool& isPreset) override {
    isPreset = false;
    if (call(OpGetChunk, 0, 0, 0, nullptr, 0, &out) && lastResult_ && !out.empty()) return true;
    isPreset = true;
    return call(OpGetChunk, 1, 0, 0, nullptr, 0, &out) && lastResult_ && !out.empty();
  }
  bool setChunk(const std::vector<uint8_t>& d, bool isPreset) override {
    return call(OpSetChunk, isPreset ? 1 : 0, 0, 0, d.data(), d.size(), nullptr, 60000) && lastResult_;
  }

  float getParameter(int32_t i) override {
    return (sh_ && i >= 0 && i < (int32_t)kParamCache) ? sh_->paramCache[i] : 0.f;
  }
  void setParameter(int32_t i, float v) override {
    if (!sh_ || i < 0) return;
    if (i < (int32_t)kParamCache) sh_->paramCache[i] = v;
    if (GetCurrentThreadId() == mainThread_ && !audioRunning()) {
      call(OpSetParameter, i, 0, v);
      return;
    }
    // From the audio thread (or while audio runs): ride along with the next block.
    std::lock_guard<std::mutex> lk(paramM_);
    for (auto& p : pendingParams_)
      if (p.index == i) {
        p.value = v;
        return;
      }
    if (pendingParams_.size() < kMaxParamChanges) pendingParams_.push_back({i, v});
  }

  bool process(float** in, float** out, int32_t frames, const vst2::VstEvents* events,
               const vst2::VstTimeInfo& time) override;

  bool editorSize(int& w, int& h) override {
    w = h = 0;
    if (!info_.hasEditor || !call(OpEditorRect)) return false;
    w = (lastResult_ >> 16) & 0xFFFF;
    h = lastResult_ & 0xFFFF;
    return w > 0 && h > 0;
  }
  bool editorOpen(void* parent) override {
    if (!call(OpEditorOpen, 0, (int64_t)(intptr_t)parent, 0, nullptr, 0, nullptr, 30000) || !lastResult_) return false;
    editorOpen_ = true;
    return true;
  }
  void editorClose() override {
    if (!editorOpen_) return;
    editorOpen_ = false;
    call(OpEditorClose);
  }
  void editorIdle() override {}  // the bridge idles its editor itself

 private:
  bool call(uint32_t op, int32_t index = 0, int64_t value = 0, double dvalue = 0, const void* data = nullptr,
            size_t size = 0, std::vector<uint8_t>* out = nullptr, DWORD timeoutMs = 15000);
  std::string callString(uint32_t op, int32_t index) {
    std::vector<uint8_t> out;
    if (!call(op, index, 0, 0, nullptr, 0, &out)) return {};
    return std::string((const char*)out.data(), out.size());
  }
  void copyInfo() {
    Info& i = sh_->info;
    info_.name.assign(i.name, strnlen(i.name, sizeof i.name));
    info_.vendor.assign(i.vendor, strnlen(i.vendor, sizeof i.vendor));
    info_.product.assign(i.product, strnlen(i.product, sizeof i.product));
    info_.uniqueId = i.uniqueId;
    info_.numInputs = i.numInputs;
    info_.numOutputs = i.numOutputs;
    info_.numParams = i.numParams;
    info_.numPrograms = i.numPrograms;
    info_.initialDelay = i.initialDelay;
    info_.tailSamples = i.tailSamples;
    info_.isSynth = i.isSynth != 0;
    info_.hasEditor = i.hasEditor != 0;
    info_.usesChunks = i.usesChunks != 0;
  }
  bool audioRunning() const { return GetTickCount64() - lastProcessMs_.load() < 200; }
  void notifyLoop();
  void shutdown();

  // MIDI the plugin produced in the block just finished: hand it to the listener as VST events,
  // the same way an in-process plugin delivers it.
  void forwardMidiOut(const Audio& a, int32_t offset) {
    if (!listener_ || (a.numOutMidi <= 0 && a.numOutSysex <= 0)) return;
    int n = std::min<int>(a.numOutMidi, kMaxMidi), ns = std::min<int>(a.numOutSysex, kMaxSysex);
    outMidi_.resize(kMaxMidi);
    outSysex_.resize(kMaxSysex);
    outEvStore_.resize(sizeof(vst2::VstEvents) + sizeof(void*) * (kMaxMidi + kMaxSysex));
    auto* evs = reinterpret_cast<vst2::VstEvents*>(outEvStore_.data());
    evs->numEvents = 0;
    for (int i = 0; i < n; ++i) {
      auto& m = outMidi_[i];
      memset(&m, 0, sizeof m);
      m.type = vst2::kVstMidiType;
      m.byteSize = sizeof m;
      m.deltaFrames = a.outMidi[i].deltaFrames + offset;
      memcpy(m.midiData, a.outMidi[i].bytes, 4);
      evs->events[evs->numEvents++] = reinterpret_cast<vst2::VstEvent*>(&m);
    }
    for (int i = 0; i < ns; ++i) {
      auto& r = a.outSysex[i];
      if (r.offset + r.size > kSysexBytes) continue;
      auto& x = outSysex_[i];
      memset(&x, 0, sizeof x);
      x.type = vst2::kVstSysExType;
      x.byteSize = sizeof x;
      x.deltaFrames = r.deltaFrames + offset;
      x.dumpBytes = (int32_t)r.size;
      x.sysexDump = (char*)a.outSysexData + r.offset;
      evs->events[evs->numEvents++] = reinterpret_cast<vst2::VstEvent*>(&x);
    }
    if (evs->numEvents) listener_->vstMidiOut(evs);
  }
  std::vector<vst2::VstMidiEvent> outMidi_;
  std::vector<vst2::VstMidiSysexEvent> outSysex_;
  std::vector<uint8_t> outEvStore_;

  Listener* listener_;
  DWORD mainThread_ = 0;
  std::string base_;
  HANDLE map_ = nullptr, ctrlReq_ = nullptr, ctrlResp_ = nullptr, audioReq_ = nullptr, audioResp_ = nullptr,
         notify_ = nullptr, proc_ = nullptr;
  Shared* sh_ = nullptr;
  HostedInfo info_;
  std::mutex callM_;
  uint32_t ctrlSeq_ = 0;
  int32_t lastResult_ = 0;
  std::atomic<bool> broken_{false};
  std::atomic<bool> stop_{false};
  std::thread notifyThread_;
  bool editorOpen_ = false;

  // audio thread
  uint32_t audioSeq_ = 0;
  bool audioPending_ = false;
  double sr_ = 44100;
  std::atomic<uint64_t> lastProcessMs_{0};
  std::mutex paramM_;
  std::vector<ParamChange> pendingParams_;
};

bool BridgedPlugin::call(uint32_t op, int32_t index, int64_t value, double dvalue, const void* data, size_t size,
                         std::vector<uint8_t>* out, DWORD timeoutMs) {
  std::lock_guard<std::mutex> lk(callM_);
  if (!sh_ || broken_.load() || (proc_ && WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0)) return false;
  Ctrl& c = sh_->ctrl;
  HANDLE bigReq = nullptr;
  c.op = op;
  c.index = index;
  c.value = value;
  c.dvalue = dvalue;
  c.bigSize = 0;
  c.dataSize = 0;
  c.result = 0;
  if (size > kCtrlDataSize) {
    char nm[96];
    snprintf(nm, sizeof nm, "%s-req%u", base_.c_str(), ctrlSeq_ + 1);
    bigReq = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32),
                                (DWORD)size, nm);
    if (!bigReq) return false;
    void* p = MapViewOfFile(bigReq, FILE_MAP_WRITE, 0, 0, size);
    if (!p) {
      CloseHandle(bigReq);
      return false;
    }
    memcpy(p, data, size);
    UnmapViewOfFile(p);
    snprintf(c.bigName, sizeof c.bigName, "%s", nm);
    c.bigSize = (uint32_t)size;
  } else if (size) {
    memcpy(c.data, data, size);
    c.dataSize = (uint32_t)size;
  }
  c.seq = ++ctrlSeq_;
  WaitForSingleObject(ctrlResp_, 0);  // drop a stale signal
  MemoryBarrier();
  SetEvent(ctrlReq_);

  // Wait, but keep handling messages sent to this thread: the bridge's editor window is a child
  // of ours, and Windows sends it messages synchronously across processes.
  HANDLE waits[2] = {ctrlResp_, proc_};
  ULONGLONG deadline = GetTickCount64() + timeoutMs;
  bool ok = false;
  for (;;) {
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) break;
    DWORD r = MsgWaitForMultipleObjects(2, waits, FALSE, (DWORD)(deadline - now), QS_SENDMESSAGE);
    if (r == WAIT_OBJECT_0) {
      ok = true;
      break;
    }
    if (r == WAIT_OBJECT_0 + 1) break;  // bridge process died
    if (r == WAIT_OBJECT_0 + 2) {
      MSG m;
      PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      continue;
    }
    break;
  }
  if (bigReq) CloseHandle(bigReq);
  if (!ok) {
    if (WaitForSingleObject(proc_, 0) != WAIT_OBJECT_0) {
      logLine("bridge did not answer op %u within %lu ms; shutting it down", op, timeoutMs);
      broken_ = true;
      TerminateProcess(proc_, 0xDEAD);
    }
    return false;
  }
  MemoryBarrier();
  lastResult_ = c.result;
  if (out) {
    out->clear();
    if (c.bigSize) {
      HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, c.bigName);
      if (m) {
        if (void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, c.bigSize)) {
          out->assign((uint8_t*)p, (uint8_t*)p + c.bigSize);
          UnmapViewOfFile(p);
        }
        CloseHandle(m);
      }
    } else {
      out->assign(c.data, c.data + std::min(c.dataSize, kCtrlDataSize));
    }
  }
  return true;
}

bool BridgedPlugin::start(const std::string& path, int32_t shellId, bool is32, std::string& error) {
  std::string exe = helperExePath(is32 ? PeArch::X86 : PeArch::X64);
  if (exe.empty()) {
    error = is32 ? "the 32-bit helper (YeOldeVST-Host32.exe) is missing" : "the 64-bit helper is missing";
    return false;
  }
  static std::atomic<uint32_t> counter{0};
  char nm[160];
  snprintf(nm, sizeof nm, "Local\\YeOldeVST-%lu-%u-%llu", GetCurrentProcessId(), ++counter,
           (unsigned long long)GetTickCount64());
  base_ = nm;
  size_t size = sizeof(Shared);
  map_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32), (DWORD)size,
                            base_.c_str());
  if (!map_) {
    error = "could not create shared memory";
    return false;
  }
  sh_ = (Shared*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!sh_) {
    error = "could not map shared memory";
    return false;
  }
  memset(sh_, 0, offsetof(Shared, ctrl));
  sh_->magic = kMagic;
  sh_->version = kVersion;
  sh_->clientPid = GetCurrentProcessId();
  auto ev = [&](const char* suffix) {
    char en[200];
    objectName(en, sizeof en, base_.c_str(), suffix);
    return CreateEventA(nullptr, FALSE, FALSE, en);
  };
  ctrlReq_ = ev("-creq");
  ctrlResp_ = ev("-cresp");
  audioReq_ = ev("-areq");
  audioResp_ = ev("-aresp");
  notify_ = ev("-notify");
  if (!ctrlReq_ || !ctrlResp_ || !audioReq_ || !audioResp_ || !notify_) {
    error = "could not create events";
    return false;
  }

  std::wstring cmd = L"\"" + widen(exe) + L"\" --bridge " + widen(base_);
  std::wstring dir = widen(dirOf(path));
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
    error = "could not start the helper process (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  if (HANDLE job = bridgeJob()) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  proc_ = pi.hProcess;
  // The helper may show the plugin's dialogs: let it take the foreground.
  AllowSetForegroundWindow(pi.dwProcessId);

  notifyThread_ = std::thread([this] { notifyLoop(); });

  if (!call(OpHello, 0, 0, 0, nullptr, 0, nullptr, 20000)) {
    error = "the helper process did not start: " + deathReason();
    return false;
  }
  std::vector<uint8_t> out;
  // Loading can show licence / registration dialogs: give the user time.
  if (!call(OpLoad, shellId, 0, 0, path.data(), path.size(), &out, 300000)) {
    error = "the plugin crashed or hung while loading: " + deathReason();
    return false;
  }
  if (!lastResult_) {
    error = std::string((const char*)out.data(), out.size());
    if (error.empty()) error = "the plugin could not be loaded";
    return false;
  }
  copyInfo();
  logLine("bridge %s up for %s (pid %lu)", is32 ? "x86" : "x64", path.c_str(), pi.dwProcessId);
  return true;
}

void BridgedPlugin::notifyLoop() {
  while (!stop_.load()) {
    WaitForSingleObject(notify_, 100);
    if (!sh_) continue;
    while (sh_->ringRead != sh_->ringWrite && !stop_.load()) {
      MemoryBarrier();
      Notify n = sh_->ring[sh_->ringRead % kRingSize];
      sh_->ringRead = sh_->ringRead + 1;
      if (!listener_) continue;
      switch (n.type) {
        case NAutomate: listener_->vstAutomate(n.index, n.value); break;
        case NBeginEdit: listener_->vstBeginEdit(n.index); break;
        case NEndEdit: listener_->vstEndEdit(n.index); break;
        case NSizeWindow: listener_->vstSizeWindow(n.w, n.h); break;
        case NIOChanged: listener_->vstIOChanged(); break;
        case NUpdateDisplay: listener_->vstUpdateDisplay(); break;
      }
    }
  }
}

bool BridgedPlugin::process(float** in, float** out, int32_t frames, const vst2::VstEvents* events,
                            const vst2::VstTimeInfo& time) {
  if (!sh_ || dead()) return false;
  lastProcessMs_ = GetTickCount64();
  Audio& a = sh_->audio;
  if (audioPending_) {
    // The previous block timed out. Skip until the bridge has caught up.
    if (a.doneSeq != audioSeq_) return false;
    audioPending_ = false;
  }
  int nin = std::clamp(info_.numInputs, 0, (int)kMaxChannels);
  int nout = std::clamp(info_.numOutputs, 0, (int)kMaxChannels);
  // Time budget per block: generous, but bounded so a stuck bridge can't stall the DAW for long.
  for (int32_t offset = 0; offset < frames;) {
    int32_t n = std::min<int32_t>(frames - offset, (int32_t)kMaxFrames);
    a.frames = n;
    a.numParams = 0;
    if (offset == 0) {
      std::unique_lock<std::mutex> lk(paramM_, std::try_to_lock);
      if (lk.owns_lock()) {
        for (auto& p : pendingParams_) a.params[a.numParams++] = p;
        pendingParams_.clear();
      }
    }
    a.numMidi = 0;
    a.numSysex = 0;
    uint32_t sysexUsed = 0;
    if (events) {
      for (int i = 0; i < events->numEvents; ++i) {
        auto* e = events->events[i];
        if (e->deltaFrames < offset || e->deltaFrames >= offset + n) {
          if (!(offset == 0 && e->deltaFrames < 0)) continue;
        }
        if (e->type == vst2::kVstMidiType && a.numMidi < (int32_t)kMaxMidi) {
          auto* m = reinterpret_cast<const vst2::VstMidiEvent*>(e);
          MidiEvent& d = a.midi[a.numMidi++];
          d.deltaFrames = std::max(0, m->deltaFrames - offset);
          memcpy(d.bytes, m->midiData, 4);
          d.noteLength = m->noteLength;
          d.noteOffset = m->noteOffset;
          d.detune = m->detune;
          d.noteOffVelocity = (uint8_t)m->noteOffVelocity;
        } else if (e->type == vst2::kVstSysExType && a.numSysex < (int32_t)kMaxSysex) {
          auto* s = reinterpret_cast<const vst2::VstMidiSysexEvent*>(e);
          if (s->dumpBytes <= 0 || sysexUsed + (uint32_t)s->dumpBytes > kSysexBytes) continue;
          memcpy(a.sysexData + sysexUsed, s->sysexDump, (size_t)s->dumpBytes);
          a.sysex[a.numSysex++] = {std::max(0, s->deltaFrames - offset), sysexUsed, (uint32_t)s->dumpBytes};
          sysexUsed += (uint32_t)s->dumpBytes;
        }
      }
    }
    static_assert(sizeof(TimeInfo) == sizeof(vst2::VstTimeInfo), "time info layout");
    memcpy(&a.time, &time, sizeof(TimeInfo));
    a.time.samplePos += offset;
    for (int c = 0; c < nin; ++c) memcpy(a.in[c], in[c] + offset, sizeof(float) * (size_t)n);

    a.seq = ++audioSeq_;
    WaitForSingleObject(audioResp_, 0);  // drop a stale signal
    MemoryBarrier();
    SetEvent(audioReq_);
    double blockMs = n / std::max(1.0, sr_) * 1000.0;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)std::max(20.0, blockMs * 3.0);
    bool done = false;
    for (;;) {
      ULONGLONG now = GetTickCount64();
      if (a.doneSeq == audioSeq_) {
        done = true;
        break;
      }
      if (now >= deadline) break;
      WaitForSingleObject(audioResp_, (DWORD)(deadline - now));
    }
    if (!done) {
      audioPending_ = true;
      return false;
    }
    MemoryBarrier();
    forwardMidiOut(a, offset);
    if (!a.ok) return false;
    for (int c = 0; c < nout; ++c) memcpy(out[c] + offset, a.out[c], sizeof(float) * (size_t)n);
    offset += n;
  }
  return true;
}

void BridgedPlugin::shutdown() {
  if (proc_ && !dead()) {
    if (editorOpen_) editorClose();
    call(OpQuit, 0, 0, 0, nullptr, 0, nullptr, 3000);
    if (WaitForSingleObject(proc_, 3000) != WAIT_OBJECT_0) TerminateProcess(proc_, 0);
  } else if (proc_) {
    TerminateProcess(proc_, 0);
  }
  stop_ = true;
  if (notify_) SetEvent(notify_);
  if (notifyThread_.joinable()) notifyThread_.join();
  for (HANDLE h : {ctrlReq_, ctrlResp_, audioReq_, audioResp_, notify_, proc_})
    if (h) CloseHandle(h);
  if (sh_) UnmapViewOfFile(sh_);
  if (map_) CloseHandle(map_);
  sh_ = nullptr;
  map_ = nullptr;
  proc_ = nullptr;
}

}  // namespace

std::unique_ptr<HostedPlugin> loadBridged(const std::string& path, int32_t shellId, bool is32,
                                          HostedPlugin::Listener* l, std::string& error) {
  auto p = std::make_unique<BridgedPlugin>(l);
  if (!p->start(path, shellId, is32, error)) return nullptr;
  return p;
}

}  // namespace yov
