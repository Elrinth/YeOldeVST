// Headless VST3 host for YeOldeVST's MIDI tests: drives the VST3 the way a VST3 DAW sends MIDI
// controllers (as parameters found through IMidiMapping) and checks what reaches the hosted VST2.
//
//   yov_vst3_midi_test <YeOldeVST.vst3 binary> <VST2 test plugin>
//
// Every controller (CC 0-127, channel aftertouch, pitch bend) on every MIDI channel is sent at
// several values, as a host computes them in double and in float precision.
// The VST2 test plugin logs each MIDI message it receives (YOV_TEST_MIDI_LOG); the test compares
// that log with the messages the values stand for. Prints PASS or the mismatches.
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

class Host : public IHostApplication {
 public:
  tresult PLUGIN_API getName(String128 name) override {
    const char* n = "YOV VST3 test";
    for (int i = 0;; ++i) {
      name[i] = (char16)n[i];
      if (!n[i]) break;
    }
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
    *obj = nullptr;
    return kNotImplemented;
  }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (FUnknownPrivate::iidEqual(iid, IHostApplication::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
      *obj = this;
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

std::vector<std::string> readLog(const std::string& path) {
  std::vector<std::string> lines;
  if (FILE* f = fopen(path.c_str(), "r")) {
    char buf[128];
    while (fgets(buf, sizeof buf, f)) {
      std::string s(buf);
      s = s.substr(0, s.find(" @"));  // drop the sample offset
      lines.push_back(s);
    }
    fclose(f);
  }
  return lines;
}

std::string midiText(int b0, int b1, int b2) {
  char buf[16];
  snprintf(buf, sizeof buf, "%02X %02X %02X", b0, b1, b2);
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <YeOldeVST.vst3 binary> <VST2 test plugin>\n", argv[0]);
    return 2;
  }
  std::string log = std::string(argv[2]) + ".midi.log";
#if defined(_WIN32)
  if (const char* tmp = getenv("TEMP")) log = std::string(tmp) + "\\yov_vst3_midi.log";
#else
  log = "/tmp/yov_vst3_midi.log";
#endif
  remove(log.c_str());
  setEnv("YEOLDEVST_TEST_AUTOLOAD", argv[2]);
  setEnv("YOV_TEST_MIDI_LOG", log);

#if defined(_WIN32)
  HMODULE mod = LoadLibraryA(argv[1]);
  if (!mod) return fprintf(stderr, "cannot load %s\n", argv[1]), 1;
  using InitFn = bool (*)();
  if (auto init = (InitFn)GetProcAddress(mod, "InitDll")) init();
  auto getFactory = (GetFactoryProc)GetProcAddress(mod, "GetPluginFactory");
#else
  void* mod = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!mod) return fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror()), 1;
  using EntryFn = bool (*)(void*);
  if (auto entry = (EntryFn)dlsym(mod, "ModuleEntry")) entry(mod);
  auto getFactory = (GetFactoryProc)dlsym(mod, "GetPluginFactory");
#endif
  if (!getFactory) return fprintf(stderr, "no GetPluginFactory\n"), 1;
  IPluginFactory* factory = getFactory();

  // The first audio module class (YeOldeVST Instrument).
  PClassInfo ci{};
  bool found = false;
  for (int32 i = 0; i < factory->countClasses() && !found; ++i)
    if (factory->getClassInfo(i, &ci) == kResultOk && !strcmp(ci.category, kVstAudioEffectClass)) found = true;
  if (!found) return fprintf(stderr, "no audio module class\n"), 1;
  printf("class: %s\n", ci.name);

  Host host;
  IComponent* component = nullptr;
  if (factory->createInstance(ci.cid, IComponent::iid, (void**)&component) != kResultOk || !component)
    return fprintf(stderr, "createInstance failed\n"), 1;
  if (component->initialize(&host) != kResultOk) return fprintf(stderr, "initialize failed\n"), 1;

  IAudioProcessor* processor = nullptr;
  component->queryInterface(IAudioProcessor::iid, (void**)&processor);
  IMidiMapping* mapping = nullptr;
  component->queryInterface(IMidiMapping::iid, (void**)&mapping);  // clap-wrapper: one object
  if (!processor || !mapping) return fprintf(stderr, "no IAudioProcessor / IMidiMapping\n"), 1;

  constexpr int32 kFrames = 64;
  ProcessSetup setup{kRealtime, kSample32, kFrames, 48000.0};
  processor->setupProcessing(setup);
  for (int32 i = 0; i < component->getBusCount(kAudio, kOutput); ++i) component->activateBus(kAudio, kOutput, i, true);
  for (int32 i = 0; i < component->getBusCount(kEvent, kInput); ++i) component->activateBus(kEvent, kInput, i, true);
  component->setActive(true);
  processor->setProcessing(true);

  // Output buffers for every bus.
  int32 nOut = component->getBusCount(kAudio, kOutput);
  std::vector<AudioBusBuffers> outs((size_t)nOut);
  std::vector<std::vector<float>> storage;
  std::vector<std::vector<float*>> chans((size_t)nOut);
  for (int32 b = 0; b < nOut; ++b) {
    BusInfo bi{};
    component->getBusInfo(kAudio, kOutput, b, bi);
    for (int32 c = 0; c < bi.channelCount; ++c) storage.emplace_back(kFrames, 0.f);
  }
  size_t s = 0;
  for (int32 b = 0; b < nOut; ++b) {
    BusInfo bi{};
    component->getBusInfo(kAudio, kOutput, b, bi);
    for (int32 c = 0; c < bi.channelCount; ++c) chans[(size_t)b].push_back(storage[s++].data());
    outs[(size_t)b].numChannels = bi.channelCount;
    outs[(size_t)b].channelBuffers32 = chans[(size_t)b].data();
  }

  auto process = [&](ParameterChanges* changes) {
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = kFrames;
    data.numOutputs = nOut;
    data.outputs = outs.data();
    data.inputParameterChanges = changes;
    processor->process(data);
  };
  for (int i = 0; i < 4; ++i) process(nullptr);  // settle

  // One block per (channel, value): every controller on that channel at once.
  struct Ctrl {
    CtrlNumber number;
    int max;
  };
  std::vector<Ctrl> ctrls;
  for (int cc = 0; cc < 128; ++cc) ctrls.push_back({(CtrlNumber)cc, 127});
  ctrls.push_back({kAfterTouch, 127});
  ctrls.push_back({kPitchBend, 16383});

  std::vector<std::string> expected;
  int unmapped = 0;
  const int values[] = {0, 1, 2, 63, 64, 65, 100, 126, 127};
  const int bends[] = {0, 1, 4095, 8191, 8192, 8193, 12288, 16382, 16383};
  for (int precision = 0; precision < 2; ++precision) {  // 0: double, 1: float (as many hosts store it)
    for (int16 ch = 0; ch < 16; ++ch) {
      for (size_t vi = 0; vi < sizeof values / sizeof values[0]; ++vi) {
        ParameterChanges changes((int32)ctrls.size());
        for (auto& c : ctrls) {
          ParamID id = 0;
          if (mapping->getMidiControllerAssignment(0, ch, c.number, id) != kResultTrue) {
            ++unmapped;
            continue;
          }
          int v = c.max == 16383 ? bends[vi] : values[vi];
          double norm = v / double(c.max);
          if (precision) norm = (double)(float)norm;
          int32 index = 0;
          if (auto* q = changes.addParameterData(id, index)) q->addPoint(0, norm, index);
          if (c.number == kAfterTouch) expected.push_back(midiText(0xD0 | ch, v, 0));
          else if (c.number == kPitchBend) expected.push_back(midiText(0xE0 | ch, v & 0x7F, v >> 7));
          else expected.push_back(midiText(0xB0 | ch, c.number, v));
        }
        process(&changes);
      }
    }
  }
  for (int i = 0; i < 4; ++i) process(nullptr);

  processor->setProcessing(false);
  component->setActive(false);
  mapping->release();
  processor->release();
  component->terminate();
  component->release();

  // Compare as multisets: the order of controllers within a block is the host's business.
  auto got = readLog(log);
  std::map<std::string, int> diff;
  for (auto& e : expected) ++diff[e];
  for (auto& g : got) --diff[g];
  int missing = 0, extra = 0, shown = 0;
  for (auto& [msg, n] : diff) {
    if (n == 0) continue;
    if (n > 0) missing += n;
    else extra -= n;
    if (shown++ < 40) printf("%s %s x%d\n", n > 0 ? "missing" : "unexpected", msg.c_str(), n > 0 ? n : -n);
  }
  printf("sent %zu, received %zu, missing %d, unexpected %d, unmapped %d\n", expected.size(), got.size(), missing,
         extra, unmapped);
  bool pass = !missing && !extra && !unmapped && !expected.empty();
  printf("%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
