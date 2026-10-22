#include "hostexe/Probe.h"

#include "common/ModuleInfo.h"
#include "common/Seh.h"
#include "common/Util.h"
#include "host/Vst2Plugin.h"
#include "platform/Helper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vst2;

namespace yov {

namespace {

const char* gStage = "start";

void emit(const char* kind, const std::string& text) {
  std::string line = "@@YOV ";
  line += kind;
  line += ' ';
  for (char c : text) line += (c == '\n' || c == '\r') ? ' ' : c;
  line += '\n';
  helper::writeStdout(line);
}

void info(const char* key, const std::string& value) { emit("info", std::string(key) + "\t" + value); }
void info(const char* key, long long value) { info(key, std::to_string(value)); }

void stage(const char* s) {
  gStage = s;
  emit("stage", s);
}

// Debug aid: YOV_PROBE_SKIP=params,state ... skips those stages.
bool skipped(const char* s) {
  std::string v;
  if (!getEnv("YOV_PROBE_SKIP", v)) return false;
  return v.find(s) != std::string::npos;
}

// Last line of defence: something faulted outside our guards (a plugin thread, a destructor).
void unhandledFault(const SehFault& f) { emit("fault", std::string(gStage) + "\t" + f.describe()); }

struct Listener : Vst2Plugin::Listener {
  int sizeRequests = 0;
  bool vstSizeWindow(int32_t, int32_t) override {
    ++sizeRequests;
    return true;
  }
};

void pump(int ms, Vst2Plugin& p) {
  auto end = nowMs() + ms;
  while (nowMs() < end && !p.dead()) {
    helper::pumpMessages(1 << 30);
    p.editorIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
}

int finish(int code) {
  // Skip static destructors / DLL detach of whatever the plugin left behind.
  helper::exitNow(code);
}

void reportPopup(const std::string& text) { emit("popup", text); }

int faulted(Vst2Plugin& p) {
  emit("fault", p.deathStage() + "\t" + p.deathReason());
  return finish(3);
}

}  // namespace

int runProbe(const ProbeOptions& opt) {
  helper::init(true);
  setUnhandledFaultHandler(unhandledFault);

  emit("arch", archName(nativeArch()));
  helper::startPopupEater(reportPopup);
  stage("load");
  Listener listener;
  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  auto plug = Vst2Plugin::load(opt.path, opt.shellId, &listener, err);
  if (!plug) {
    emit("error", err);
    return finish(2);
  }
  auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  info("loadMs", loadMs);

  stage("info");
  auto& p = *plug;
  info("name", p.effectName());
  info("product", p.productName());
  info("vendor", p.vendorName());
  info("vendorVersion", p.vendorVersion());
  info("vstVersion", p.vstVersion());
  int32_t cat = p.category();
  info("category", cat);
  info("uniqueId", p.uniqueId());
  info("version", p.effect()->version);
  info("flags", p.effect()->flags);
  info("numInputs", p.numInputs());
  info("numOutputs", p.numOutputs());
  info("numParams", p.numParams());
  info("numPrograms", p.numPrograms());
  info("initialDelay", p.initialDelay());
  bool midiIn = p.canDo("receiveVstMidiEvent") || p.canDo("receiveVstEvents");
  info("midiInput", midiIn ? 1 : 0);
  if (p.dead()) return faulted(p);

  if (cat == kPlugCategShell && opt.shellId == 0) {
    stage("shell");
    for (int i = 0; i < 2000; ++i) {
      char name[512] = {};
      intptr_t id = p.dispatch(effShellGetNextPlugin, 0, 0, name);
      if (p.dead()) return faulted(p);
      if (id == 0 || name[0] == 0) break;
      name[sizeof name - 1] = 0;
      emit("shell", std::to_string((int32_t)id) + "\t" + name);
    }
    emit("done", "");
    return finish(0);
  }

  if (opt.editorOnly) {
    p.setSampleRate(44100);
    p.setBlockSize(512);
    p.resume();
  }
  if (!opt.editorOnly) {
  if (p.numInputs() > Vst2Plugin::kChannelLimit || p.numOutputs() > Vst2Plugin::kChannelLimit || p.numInputs() < 0 ||
      p.numOutputs() < 0)
    emit("warn", "reports an impossible number of audio channels");
  if (p.numOutputs() == 0) emit("warn", "plugin has no audio outputs");

  stage("setup");
  const int block = 512;
  const double sr = 44100;
  p.setSampleRate(sr);
  p.setBlockSize(block);
  p.resume();
  if (p.dead()) return faulted(p);

  stage("process");
  if (!skipped("process")) {
  int nin = std::max(0, std::min(p.numInputs(), 64));
  int nout = std::max(0, std::min(p.numOutputs(), 64));
  std::vector<std::vector<float>> inBuf(std::max(nin, 1), std::vector<float>(block)),
      outBuf(std::max(nout, 1), std::vector<float>(block));
  std::vector<float*> inPtr, outPtr;
  for (auto& b : inBuf) inPtr.push_back(b.data());
  for (auto& b : outBuf) outPtr.push_back(b.data());
  uint32_t rng = 12345;
  bool badSamples = false;
  float peak = 0;
  double procSec = 0;
  const int blocks = 64;
  for (int b = 0; b < blocks; ++b) {
    for (auto& ch : inBuf)
      for (auto& s : ch) {
        rng = rng * 1664525u + 1013904223u;
        s = ((rng >> 9) * (1.0f / 8388608.0f) - 0.5f) * 0.1f;
      }
    if (midiIn || p.isSynth()) {
      // Note on at block 2, off at block 40: a synth should make sound in between.
      VstMidiEvent ev{};
      ev.type = kVstMidiType;
      ev.byteSize = sizeof(ev);
      ev.flags = kVstMidiEventIsRealtime;
      bool send = false;
      if (b == 2) {
        ev.midiData[0] = (char)0x90;
        ev.midiData[1] = 60;
        ev.midiData[2] = 100;
        send = true;
      } else if (b == 40) {
        ev.midiData[0] = (char)0x80;
        ev.midiData[1] = 60;
        send = true;
      }
      if (send) {
        VstEvents evs{};
        evs.numEvents = 1;
        evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
        p.processEvents(&evs);
        if (p.dead()) return faulted(p);
      }
    }
    p.timeInfo().samplePos = double(b) * block;
    p.timeInfo().ppqPos = p.timeInfo().samplePos / sr * 2.0;
    auto pt = std::chrono::steady_clock::now();
    p.process(inPtr.data(), nin, outPtr.data(), nout, block);
    procSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - pt).count();
    if (p.dead()) return faulted(p);
    if (b >= 4) {  // skip warm-up blocks for the stats
      for (int c = 0; c < nout; ++c)
        for (float s : outBuf[c]) {
          if (!std::isfinite(s)) badSamples = true;
          else peak = std::max(peak, std::fabs(s));
        }
    }
  }
  double cpu = procSec / (double(blocks) * block / sr) * 100.0;
  char cpuStr[32];
  snprintf(cpuStr, sizeof cpuStr, "%.2f", cpu);
  info("cpuPercent", cpuStr);
  char peakStr[32];
  snprintf(peakStr, sizeof peakStr, "%.4f", peak);
  info("peak", peakStr);
  if (badSamples) emit("warn", "outputs NaN/infinite samples");
  if (peak > 64.f) emit("warn", "output is extremely loud (peak > +36 dB)");
  if (cpu > 50) emit("warn", "very CPU hungry (" + std::string(cpuStr) + "% of one core)");

  }
  stage("params");
  if (!skipped("params")) {
  int np = std::min(p.numParams(), 4096);
  for (int i = 0; i < np && !p.dead(); ++i) {
    p.dispatchString(effGetParamName, i);
    p.dispatchString(effGetParamDisplay, i);
    p.dispatchString(effGetParamLabel, i);
    p.getParameter(i);
  }
  if (p.dead()) return faulted(p);

  }
  stage("programs");
  if (!skipped("programs")) {
  int nprog = std::min(p.numPrograms(), 512);
  for (int i = 0; i < nprog && !p.dead(); ++i) p.dispatchString(effGetProgramNameIndexed, i);
  if (nprog > 1) {
    p.dispatch(effSetProgram, 0, 1);
    p.dispatch(effSetProgram, 0, 0);
  }
  if (p.dead()) return faulted(p);

  }
  stage("state");
  if (!skipped("state")) {
  if (p.usesChunks()) {
    // Same strategy as the plugin's state save: bank chunk first, program chunk as fallback.
    std::vector<uint8_t> chunk;
    bool preset = false;
    if (!p.getChunk(false, chunk) && !p.dead()) preset = p.getChunk(true, chunk);
    if (!chunk.empty()) {
      info("chunkBytes", (long long)chunk.size());
      info("chunkType", preset ? "program" : "bank");
      p.setChunk(preset, chunk.data(), chunk.size());
    } else if (!p.dead()) {
      emit("warn", "says it saves state as a chunk but returned none");
    }
  }
  if (p.dead()) return faulted(p);

  }
  }  // !editorOnly

  if ((opt.testEditor || opt.editorOnly) && p.hasEditor()) {
    stage("editor");
    void* wnd = helper::probeWindowCreate();
    if (!wnd) {
      emit("error", "no display to open the editor on");
      return finish(2);
    }
    int w = 0, h = 0;
    p.editorGetRect(w, h);
    if (!p.editorOpen(wnd)) {
      if (p.dead()) return faulted(p);
      emit("warn", "editor did not open");
    } else {
      int w2 = 0, h2 = 0;
      p.editorGetRect(w2, h2);
      info("editorWidth", w2);
      info("editorHeight", h2);
      if (w2 <= 0 || h2 <= 0) emit("warn", "editor reports no size");
      pump(400, p);
      if (p.dead()) return faulted(p);
      stage("editor-close");
      p.editorClose();
      if (p.dead()) return faulted(p);
    }
    helper::probeWindowDestroy(wnd);
  }

  stage("suspend");
  p.suspend();
  if (p.dead()) return faulted(p);

  stage("close");
  plug.reset();  // effClose + FreeLibrary, guarded
  emit("done", "");
  return finish(0);
}

}  // namespace yov
