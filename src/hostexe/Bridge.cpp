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

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace yov::bridge;

namespace yov {

namespace {

Shared* gShared = nullptr;

LONG WINAPI bridgeUnhandled(EXCEPTION_POINTERS* ep) {
  if (gShared) {
    SehFault f;
    f.code = ep->ExceptionRecord->ExceptionCode;
    f.address = ep->ExceptionRecord->ExceptionAddress;
    std::string d = f.describe();
    snprintf(gShared->deathReason, sizeof gShared->deathReason, "%s", d.c_str());
    gShared->dead = 1;
  }
  TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT CALLBACK containerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_ERASEBKGND) return 1;
  return DefWindowProcW(h, m, w, l);
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
    if (container_) SetWindowPos(container_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    push({NSizeWindow, 0, 0, w, h});
    return true;
  }
  void vstIOChanged() override { push({NIOChanged, 0, 0, 0, 0}); }
  void vstUpdateDisplay() override { push({NUpdateDisplay, 0, 0, 0, 0}); }

 private:
  void push(const Notify& n) {
    while (InterlockedCompareExchange((volatile LONG*)&sh_->ringLock, 1, 0) != 0) YieldProcessor();
    if (sh_->ringWrite - sh_->ringRead < kRingSize) {
      sh_->ring[sh_->ringWrite % kRingSize] = n;
      MemoryBarrier();
      sh_->ringWrite = sh_->ringWrite + 1;
    }
    InterlockedExchange((volatile LONG*)&sh_->ringLock, 0);
    SetEvent(notify_);
  }

  void checkDead() {
    if (plug_ && plug_->dead() && !sh_->dead) {
      std::string r = plug_->deathReason() + " (in " + plug_->deathStage() + ")";
      snprintf(sh_->deathReason, sizeof sh_->deathReason, "%s", r.c_str());
      MemoryBarrier();
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
      HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, c.bigName);
      if (m) {
        if (void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, c.bigSize)) {
          v.assign((uint8_t*)p, (uint8_t*)p + c.bigSize);
          UnmapViewOfFile(p);
        }
        CloseHandle(m);
      }
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
    char nm[96];
    snprintf(nm, sizeof nm, "%s-big%u", base_.c_str(), c.seq);
    bigReply_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)n >> 32),
                                   (DWORD)n, nm);
    if (!bigReply_) return;
    if (void* p = MapViewOfFile(bigReply_, FILE_MAP_WRITE, 0, 0, n)) {
      memcpy(p, data, n);
      UnmapViewOfFile(p);
      snprintf(c.bigName, sizeof c.bigName, "%s", nm);
      c.bigSize = (uint32_t)n;
    }
  }
  void reply(const std::string& s) { reply(s.data(), s.size()); }

  void handleCtrl();
  void audioLoop();

  std::string base_;
  Shared* sh_ = nullptr;
  HANDLE map_ = nullptr, ctrlReq_ = nullptr, ctrlResp_ = nullptr, audioReq_ = nullptr, audioResp_ = nullptr,
         notify_ = nullptr, parent_ = nullptr;
  HANDLE bigReply_ = nullptr;
  std::unique_ptr<Vst2Plugin> plug_;
  HWND container_ = nullptr;
  std::wstring containerClass_;
  std::atomic<bool> quit_{false};
};

void Server::handleCtrl() {
  Ctrl& c = sh_->ctrl;
  if (bigReply_) {
    CloseHandle(bigReply_);
    bigReply_ = nullptr;
  }
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
      HWND parent = (HWND)(intptr_t)c.value;
      int w = 0, h = 0;
      plug_->editorGetRect(w, h);
      container_ = CreateWindowExW(WS_EX_NOPARENTNOTIFY, containerClass_.c_str(), L"",
                                   WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, std::max(w, 1),
                                   std::max(h, 1), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!container_ || !plug_->editorOpen(container_)) {
        if (container_) DestroyWindow(container_);
        container_ = nullptr;
        break;
      }
      if (plug_->editorGetRect(w, h) && w > 0 && h > 0)
        SetWindowPos(container_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      if (w <= 0 || h <= 0) {
        HWND child = GetWindow(container_, GW_CHILD);
        RECT r{};
        if (child && GetWindowRect(child, &r)) {
          w = r.right - r.left;
          h = r.bottom - r.top;
          SetWindowPos(container_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      c.result = (std::max(w, 1) << 16) | (std::max(h, 1) & 0xFFFF);
      break;
    }
    case OpEditorClose:
      if (plug_ && plug_->editorIsOpen() && !plug_->dead()) plug_->editorClose();
      if (container_) DestroyWindow(container_);
      container_ = nullptr;
      c.dataSize = 0;
      break;
    case OpRefreshParams:
      refreshParamCache();
      c.dataSize = 0;
      break;
    case OpQuit:
      if (plug_ && plug_->editorIsOpen() && !plug_->dead()) plug_->editorClose();
      if (container_) DestroyWindow(container_);
      container_ = nullptr;
      quit_ = true;
      c.result = 1;
      c.dataSize = 0;
      break;
  }
  checkDead();
  MemoryBarrier();
  SetEvent(ctrlResp_);
}

void Server::audioLoop() {
  // Ask the scheduler for audio-thread treatment.
  if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
    using Fn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    if (auto f = (Fn)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) {
      DWORD idx = 0;
      f(L"Pro Audio", &idx);
    }
  }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

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
    if (WaitForSingleObject(audioReq_, 200) != WAIT_OBJECT_0) continue;
    uint32_t seq = a.seq;
    MemoryBarrier();
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
    MemoryBarrier();
    a.doneSeq = seq;
    SetEvent(audioResp_);
  }
}

int Server::run(const std::string& name) {
  base_ = name;
  map_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!map_) return 10;
  sh_ = (Shared*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
  if (!sh_ || sh_->magic != kMagic || sh_->version != kVersion) return 11;
  gShared = sh_;
  SetUnhandledExceptionFilter(bridgeUnhandled);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  char nm[160];
  auto openEv = [&](const char* suffix) {
    objectName(nm, sizeof nm, name.c_str(), suffix);
    return OpenEventA(EVENT_ALL_ACCESS, FALSE, nm);
  };
  ctrlReq_ = openEv("-creq");
  ctrlResp_ = openEv("-cresp");
  audioReq_ = openEv("-areq");
  audioResp_ = openEv("-aresp");
  notify_ = openEv("-notify");
  parent_ = OpenProcess(SYNCHRONIZE, FALSE, sh_->clientPid);
  if (!ctrlReq_ || !ctrlResp_ || !audioReq_ || !audioResp_ || !notify_ || !parent_) return 12;

  containerClass_ = L"YeOldeVSTBridgeEditor";
  WNDCLASSW wc{};
  wc.lpfnWndProc = containerProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = containerClass_.c_str();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  sh_->bridgePid = GetCurrentProcessId();
  sh_->ready = 1;
  std::thread audio([this] { audioLoop(); });

  HANDLE waits[2] = {ctrlReq_, parent_};
  ULONGLONG lastIdle = 0, lastParams = 0;
  while (!quit_.load()) {
    DWORD r = MsgWaitForMultipleObjects(2, waits, FALSE, 15, QS_ALLINPUT);
    if (r == WAIT_OBJECT_0) {
      handleCtrl();
    } else if (r == WAIT_OBJECT_0 + 1) {
      break;  // the DAW went away
    } else if (r == WAIT_OBJECT_0 + 2) {
      MSG msg;
      int budget = 200;
      while (budget-- > 0 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    ULONGLONG now = GetTickCount64();
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
  TerminateProcess(GetCurrentProcess(), 0);  // skip DLL teardown of whatever the plugin left
  return 0;
}

}  // namespace

int runBridge(const std::string& name) {
  static Server server;  // keeps the plugin alive until TerminateProcess
  return server.run(name);
}

}  // namespace yov
