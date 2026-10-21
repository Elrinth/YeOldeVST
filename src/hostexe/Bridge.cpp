// Bridge server: hosts one VST2 plugin for a YeOldeVST instance running in another process.
//
// Main thread: control requests + the plugin's editor (it pumps window messages).
// Audio thread: processes blocks on request.
// See common/BridgeProtocol.h for the shared-memory layout.
#include "hostexe/Bridge.h"

#include "common/BridgeProtocol.h"
#include "common/Seh.h"
#include "common/Util.h"
#include "host/Vst2Plugin.h"
#include "platform/Helper.h"
#include "platform/Ipc.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace yov::bridge;

namespace yov {

namespace {

Shared* gShared = nullptr;

void bridgeUnhandled(const SehFault& f) {
  if (gShared) {
    std::string d = f.describe();
    snprintf(gShared->deathReason, sizeof gShared->deathReason, "%s", d.c_str());
    gShared->dead = 1;
  }
}

class Server final : public Vst2Plugin::Listener {
 public:
  int run(const std::string& name);

  // Listener
  void vstAutomate(int32_t index, float value) override {
    if (index >= 0 && index < (int32_t)kParamCache) sh_->paramCache[index] = value;
    push({NAutomate, index, value, 0, 0});
  }
  void vstBeginEdit(int32_t index) override { push({NBeginEdit, index, 0, 0, 0}); }
  void vstEndEdit(int32_t index) override { push({NEndEdit, index, 0, 0, 0}); }
  bool vstSizeWindow(int32_t w, int32_t h) override {
    if (container_) helper::containerResize(container_, w, h);
    push({NSizeWindow, 0, 0, w, h});
    return true;
  }
  void vstIOChanged() override { push({NIOChanged, 0, 0, 0, 0}); }
  void vstUpdateDisplay() override { push({NUpdateDisplay, 0, 0, 0, 0}); }
  // MIDI from the plugin can come from any thread: collect it, the audio thread hands it over
  // with the next finished block.
  void vstMidiOut(const vst2::VstEvents* e) override {
    if (!e) return;
    std::lock_guard<std::mutex> lk(midiOutM_);
    for (int32_t i = 0; i < e->numEvents; ++i) {
      const vst2::VstEvent* ev = e->events[i];
      if (!ev) continue;
      if (ev->type == vst2::kVstMidiType && midiOut_.size() < kMaxMidi) {
        auto m = reinterpret_cast<const vst2::VstMidiEvent*>(ev);
        MidiEvent o{};
        o.deltaFrames = std::max(0, m->deltaFrames);
        memcpy(o.bytes, m->midiData, 4);
        midiOut_.push_back(o);
      } else if (ev->type == vst2::kVstSysExType && sysexOut_.size() < kMaxSysex) {
        auto x = reinterpret_cast<const vst2::VstMidiSysexEvent*>(ev);
        if (!x->sysexDump || x->dumpBytes <= 0 || x->dumpBytes > (int32_t)kSysexBytes) continue;
        sysexOut_.push_back({std::max(0, x->deltaFrames),
                             std::vector<uint8_t>((uint8_t*)x->sysexDump, (uint8_t*)x->sysexDump + x->dumpBytes)});
      }
    }
  }

 private:
  void push(const Notify& n) {
    std::atomic_ref<int32_t> lock(sh_->ringLock);
    int32_t expected = 0;
    while (!lock.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
      expected = 0;
      std::this_thread::yield();
    }
    if (sh_->ringWrite - sh_->ringRead < kRingSize) {
      sh_->ring[sh_->ringWrite % kRingSize] = n;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      sh_->ringWrite = sh_->ringWrite + 1;
    }
    lock.store(0, std::memory_order_release);
    notify_.signal();
  }

  void checkDead() {
    if (plug_ && plug_->dead() && !sh_->dead) {
      std::string r = plug_->deathReason() + " (in " + plug_->deathStage() + ")";
      snprintf(sh_->deathReason, sizeof sh_->deathReason, "%s", r.c_str());
      std::atomic_thread_fence(std::memory_order_seq_cst);
      sh_->dead = 1;
    }
  }

  void fillInfo() {
    Info& i = sh_->info;
    auto& p = *plug_;
    snprintf(i.name, sizeof i.name, "%s", p.effectName().c_str());
    snprintf(i.vendor, sizeof i.vendor, "%s", p.vendorName().c_str());
    snprintf(i.product, sizeof i.product, "%s", p.productName().c_str());
    i.uniqueId = p.uniqueId();
    i.numInputs = std::clamp(p.numInputs(), 0, (int)kMaxChannels);
    i.numOutputs = std::clamp(p.numOutputs(), 0, (int)kMaxChannels);
    i.numParams = std::max(0, p.numParams());
    i.numPrograms = std::max(0, p.numPrograms());
    i.initialDelay = std::max(0, p.initialDelay());
    i.tailSamples = (int32_t)std::max<intptr_t>(0, p.dispatch(vst2::effGetTailSize));
    i.isSynth = p.isSynth();
    i.hasEditor = p.hasEditor();
    i.usesChunks = p.usesChunks();
  }

  void refreshParamCache() {
    if (!plug_ || plug_->dead()) return;
    int n = std::min<int>(plug_->numParams(), (int)kParamCache);
    for (int i = 0; i < n; ++i) sh_->paramCache[i] = plug_->getParameter(i);
  }

  std::vector<uint8_t> requestData() {
    Ctrl& c = sh_->ctrl;
    std::vector<uint8_t> v;
    if (c.bigSize) {
      SharedMemory m;
      if (m.open(std::string(c.bigName, strnlen(c.bigName, sizeof c.bigName)), c.bigSize, true))
        v.assign((uint8_t*)m.data(), (uint8_t*)m.data() + c.bigSize);
    } else {
      v.assign(c.data, c.data + std::min(c.dataSize, kCtrlDataSize));
    }
    return v;
  }

  void reply(const void* data, size_t n) {
    Ctrl& c = sh_->ctrl;
    c.bigSize = 0;
    c.dataSize = 0;
    if (n <= kCtrlDataSize) {
      if (n) memcpy(c.data, data, n);
      c.dataSize = (uint32_t)n;
      return;
    }
    // Too big for the control block: a separate mapping, kept until the next request.
    std::string nm = ipcName(base_, ("-big" + std::to_string(c.seq)).c_str());
    if (!bigReply_.create(nm, n)) return;
    memcpy(bigReply_.data(), data, n);
    snprintf(c.bigName, sizeof c.bigName, "%s", nm.c_str());
    c.bigSize = (uint32_t)n;
  }
  void reply(const std::string& s) { reply(s.data(), s.size()); }

  void closeContainer() {
    if (container_) helper::containerDestroy(container_);
    container_ = nullptr;
  }

  void handleCtrl();
  void audioLoop();

  std::string base_;
  Shared* sh_ = nullptr;
  SharedMemory map_;
  IpcEvent ctrlReq_, ctrlResp_, audioReq_, audioResp_, notify_;
  ProcessWatch parent_;
  SharedMemory bigReply_;
  std::unique_ptr<Vst2Plugin> plug_;
  void* container_ = nullptr;
  std::atomic<bool> quit_{false};
  std::mutex midiOutM_;
  std::vector<MidiEvent> midiOut_;
  std::vector<std::pair<int32_t, std::vector<uint8_t>>> sysexOut_;
};

void Server::handleCtrl() {
  Ctrl& c = sh_->ctrl;
  bigReply_.close();
  c.result = 0;
  c.bigSize = 0;
  uint32_t op = c.op;
  bool loaded = plug_ && !plug_->dead();
  switch (op) {
    case OpHello:
      c.result = 1;
      c.dataSize = 0;
      break;
    case OpLoad: {
      auto d = requestData();
      std::string path((const char*)d.data(), d.size());
      std::string err;
      plug_ = Vst2Plugin::load(path, c.index, this, err);
      if (plug_) {
        fillInfo();
        refreshParamCache();
        c.result = 1;
        reply("", 0);
      } else {
        reply(err);
      }
      break;
    }
    case OpRefreshInfo:
      if (loaded) fillInfo();
      c.result = loaded;
      c.dataSize = 0;
      break;
    case OpSetup:
      if (loaded) {
        bool was = plug_->resumed();
        if (was) plug_->suspend();
        plug_->setSampleRate(c.dvalue);
        plug_->setBlockSize(std::min<int32_t>(c.index, (int32_t)kMaxFrames));
        if (was) plug_->resume();
      }
      c.dataSize = 0;
      break;
    case OpResume:
      if (loaded) plug_->resume();
      c.dataSize = 0;
      break;
    case OpSuspend:
      if (loaded) plug_->suspend();
      c.dataSize = 0;
      break;
    case OpParamName:
      reply(loaded ? plug_->dispatchString(vst2::effGetParamName, c.index) : std::string());
      break;
    case OpParamNames: {
      std::string all;
      if (loaded)
        for (int i = 0; i < std::min(c.index, plug_->numParams()); ++i) {
          all += plug_->dispatchString(vst2::effGetParamName, i);
          all += '\0';
        }
      reply(all);
      break;
    }
    case OpParamDisplay: {
      std::string s;
      if (loaded) {
        s = plug_->dispatchString(vst2::effGetParamDisplay, c.index);
        auto l = plug_->dispatchString(vst2::effGetParamLabel, c.index);
        if (!l.empty()) s += " " + l;
      }
      reply(s);
      break;
    }
    case OpGetProgram:
      c.result = loaded ? (int32_t)plug_->dispatch(vst2::effGetProgram) : 0;
      c.dataSize = 0;
      break;
    case OpSetProgram:
      if (loaded) {
        plug_->dispatch(vst2::effBeginSetProgram);
        plug_->dispatch(vst2::effSetProgram, 0, c.index);
        plug_->dispatch(vst2::effEndSetProgram);
        refreshParamCache();
      }
      c.dataSize = 0;
      break;
    case OpProgramNames: {
      std::string all;
      if (loaded) {
        int n = std::min(plug_->numPrograms(), 1024);
        int cur = (int)plug_->dispatch(vst2::effGetProgram);
        for (int i = 0; i < n; ++i) {
          auto s = plug_->dispatchString(vst2::effGetProgramNameIndexed, i);
          if (s.empty() && i == cur) s = plug_->dispatchString(vst2::effGetProgramName);
          all += s;
          all += '\0';
        }
      }
      reply(all);
      break;
    }
    case OpGetChunk: {
      std::vector<uint8_t> chunk;
      if (loaded && plug_->getChunk(c.index != 0, chunk)) c.result = 1;
      reply(chunk.data(), chunk.size());
      break;
    }
    case OpSetChunk: {
      auto d = requestData();
      if (loaded && !d.empty()) c.result = plug_->setChunk(c.index != 0, d.data(), d.size()) ? 1 : 0;
      refreshParamCache();
      c.dataSize = 0;
      break;
    }
    case OpSetParameter:
      if (loaded) {
        plug_->setParameter(c.index, (float)c.dvalue);
        if (c.index >= 0 && c.index < (int32_t)kParamCache) sh_->paramCache[c.index] = plug_->getParameter(c.index);
      }
      c.dataSize = 0;
      break;
    case OpEditorRect: {
      int w = 0, h = 0;
      if (loaded && plug_->editorGetRect(w, h)) c.result = (w << 16) | (h & 0xFFFF);
      c.dataSize = 0;
      break;
    }
    case OpEditorOpen: {
      c.dataSize = 0;
      if (!loaded || plug_->editorIsOpen()) break;
      int w = 0, h = 0;
      plug_->editorGetRect(w, h);
      container_ = helper::containerCreate(c.value, w, h);
      if (!container_ || !plug_->editorOpen(container_)) {
        closeContainer();
        break;
      }
      if (plug_->editorGetRect(w, h) && w > 0 && h > 0) helper::containerResize(container_, w, h);
      if (w <= 0 || h <= 0) {
        if (helper::containerChildSize(container_, w, h)) helper::containerResize(container_, w, h);
      }
      c.result = (std::max(w, 1) << 16) | (std::max(h, 1) & 0xFFFF);
      break;
    }
    case OpEditorClose:
      if (plug_ && plug_->editorIsOpen() && !plug_->dead()) plug_->editorClose();
      closeContainer();
      c.dataSize = 0;
      break;
    case OpRefreshParams:
      refreshParamCache();
      c.dataSize = 0;
      break;
    case OpQuit:
      if (plug_ && plug_->editorIsOpen() && !plug_->dead()) plug_->editorClose();
      closeContainer();
      quit_ = true;
      c.result = 1;
      c.dataSize = 0;
      break;
  }
  checkDead();
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ctrlResp_.signal();
}

void Server::audioLoop() {
  helper::boostAudioThread();

  std::vector<vst2::VstMidiEvent> midi(kMaxMidi);
  std::vector<vst2::VstMidiSysexEvent> sysex(kMaxSysex);
  std::vector<uint8_t> evStore(sizeof(vst2::VstEvents) + sizeof(void*) * (kMaxMidi + kMaxSysex));
  float* inPtr[kMaxChannels];
  float* outPtr[kMaxChannels];
  Audio& a = sh_->audio;
  for (uint32_t c = 0; c < kMaxChannels; ++c) {
    inPtr[c] = a.in[c];
    outPtr[c] = a.out[c];
  }
  while (!quit_.load()) {
    if (!audioReq_.wait(200)) continue;
    uint32_t seq = a.seq;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    a.ok = 0;
    if (plug_ && !plug_->dead() && a.frames > 0 && a.frames <= (int32_t)kMaxFrames) {
      for (int i = 0; i < std::min<int>(a.numParams, kMaxParamChanges); ++i) {
        plug_->setParameter(a.params[i].index, a.params[i].value);
        if (a.params[i].index >= 0 && a.params[i].index < (int32_t)kParamCache)
          sh_->paramCache[a.params[i].index] = a.params[i].value;
      }
      auto* evs = reinterpret_cast<vst2::VstEvents*>(evStore.data());
      evs->numEvents = 0;
      evs->reserved = 0;
      for (int i = 0; i < std::min<int>(a.numMidi, kMaxMidi); ++i) {
        auto& src = a.midi[i];
        auto& m = midi[i];
        memset(&m, 0, sizeof m);
        m.type = vst2::kVstMidiType;
        m.byteSize = sizeof m;
        m.deltaFrames = src.deltaFrames;
        m.flags = vst2::kVstMidiEventIsRealtime;
        m.noteLength = src.noteLength;
        m.noteOffset = src.noteOffset;
        memcpy(m.midiData, src.bytes, 4);
        m.detune = (char)src.detune;
        m.noteOffVelocity = (char)src.noteOffVelocity;
        evs->events[evs->numEvents++] = reinterpret_cast<vst2::VstEvent*>(&m);
      }
      for (int i = 0; i < std::min<int>(a.numSysex, kMaxSysex); ++i) {
        auto& src = a.sysex[i];
        if (src.offset + src.size > kSysexBytes) continue;
        auto& s = sysex[i];
        memset(&s, 0, sizeof s);
        s.type = vst2::kVstSysExType;
        s.byteSize = sizeof s;
        s.deltaFrames = src.deltaFrames;
        s.dumpBytes = (int32_t)src.size;
        s.sysexDump = (char*)a.sysexData + src.offset;
        evs->events[evs->numEvents++] = reinterpret_cast<vst2::VstEvent*>(&s);
      }
      static_assert(sizeof(TimeInfo) == sizeof(vst2::VstTimeInfo), "time info layout");
      memcpy(&plug_->timeInfo(), &a.time, sizeof(TimeInfo));
      if (evs->numEvents) plug_->processEvents(evs);
      a.ok = plug_->process(inPtr, outPtr, a.frames) ? 1 : 0;
      checkDead();
    }
    {
      // Hand over MIDI the plugin sent since the last block.
      std::lock_guard<std::mutex> lk(midiOutM_);
      a.numOutMidi = (int32_t)std::min<size_t>(midiOut_.size(), kMaxMidi);
      for (int32_t i = 0; i < a.numOutMidi; ++i) a.outMidi[i] = midiOut_[i];
      a.numOutSysex = 0;
      uint32_t used = 0;
      for (auto& [delta, data] : sysexOut_) {
        if (a.numOutSysex >= (int32_t)kMaxSysex || used + data.size() > kSysexBytes) break;
        memcpy(a.outSysexData + used, data.data(), data.size());
        a.outSysex[a.numOutSysex++] = {delta, used, (uint32_t)data.size()};
        used += (uint32_t)data.size();
      }
      midiOut_.clear();
      sysexOut_.clear();
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    a.doneSeq = seq;
    audioResp_.signal();
  }
}

int Server::run(const std::string& name) {
  base_ = name;
  if (!map_.open(name, sizeof(Shared))) return 10;
  sh_ = (Shared*)map_.data();
  if (!sh_ || sh_->magic != kMagic || sh_->version != kVersion) return 11;
  gShared = sh_;
  setUnhandledFaultHandler(bridgeUnhandled);
  helper::init(false);

  IpcEvent* events[] = {&ctrlReq_, &ctrlResp_, &audioReq_, &audioResp_, &notify_};
  for (int i = 0; i < 5; ++i)
    if (!events[i]->open(ipcName(name, kWakeSuffix[i]), &sh_->wake[i])) return 12;
  if (!parent_.open(sh_->clientPid)) return 12;

  sh_->bridgePid = currentProcessId();
  sh_->ready = 1;
  std::thread audio([this] { audioLoop(); });

  uint64_t lastIdle = 0, lastParams = 0;
  while (!quit_.load()) {
    WaitResult r = waitForRequest(ctrlReq_, parent_, 15);
    if (r == WaitResult::Signaled) {
      handleCtrl();
    } else if (r == WaitResult::ProcessEnded) {
      break;  // the DAW went away
    } else if (r == WaitResult::Messages) {
      helper::pumpMessages(200);
    }
#ifndef _WIN32
    helper::pumpMessages(200);  // our own X connection (the editor container)
#endif
    uint64_t now = nowMs();
    if (now - lastIdle >= 30) {
      lastIdle = now;
      if (plug_ && plug_->editorIsOpen()) {
        plug_->editorIdle();
        checkDead();
      }
    }
    if (now - lastParams >= 250) {
      lastParams = now;
      refreshParamCache();
      checkDead();
    }
  }
  quit_ = true;
  audio.join();
  if (plug_ && !plug_->dead()) plug_.reset();
  helper::exitNow(0);  // skip DLL teardown of whatever the plugin left
}

}  // namespace

int runBridge(const std::string& name) {
  static Server server;  // keeps the plugin alive until the process ends
  return server.run(name);
}

}  // namespace yov
