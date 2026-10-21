// Tiny VST2 test plugin for YeOldeVST's own tests (built as 32-bit and 64-bit).
//
// - 6 outputs; output c carries the constant level 0.1 * (c + 1) while a note is held, so a
//   test can check every channel reaches the right DAW port.
// - Echoes every incoming note on/off back to the host through audioMasterProcessEvents,
//   transposed up an octave, at the same sample offset (tests MIDI output).
#include "vst2/aeffect.h"

#include <cstring>

using namespace vst2;

namespace {

struct TestPlugin {
  AEffect fx{};
  HostCallback host = nullptr;
  int held = 0;
  VstMidiEvent out[64];
  struct {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[64];
  } outList{};
};

TestPlugin* self(AEffect* e) { return static_cast<TestPlugin*>(e->object); }

intptr_t YOV_VSTCALL dispatcher(AEffect* e, int32_t op, int32_t, intptr_t, void* ptr, float) {
  auto* t = self(e);
  switch (op) {
    case effClose: delete t; return 1;
    case effGetEffectName:
    case effGetProductString: strcpy((char*)ptr, "YOV Test"); return 1;
    case effGetVendorString: strcpy((char*)ptr, "YeOldeVST"); return 1;
    case effGetVstVersion: return 2400;
    case effGetPlugCategory: return kPlugCategSynth;
    case effCanDo:
      return (!strcmp((const char*)ptr, "receiveVstMidiEvent") || !strcmp((const char*)ptr, "sendVstMidiEvent") ||
              !strcmp((const char*)ptr, "receiveVstEvents") || !strcmp((const char*)ptr, "sendVstEvents"))
                 ? 1
                 : 0;
    case effProcessEvents: {
      auto* evs = static_cast<VstEvents*>(ptr);
      int n = 0;
      for (int i = 0; i < evs->numEvents && n < 64; ++i) {
        if (evs->events[i]->type != kVstMidiType) continue;
        auto* m = reinterpret_cast<VstMidiEvent*>(evs->events[i]);
        uint8_t st = (uint8_t)m->midiData[0] & 0xF0;
        bool on = st == 0x90 && m->midiData[2] != 0;
        bool off = st == 0x80 || (st == 0x90 && m->midiData[2] == 0);
        if (on) ++t->held;
        if (off && t->held > 0) --t->held;
        if (!on && !off) continue;
        VstMidiEvent& o = t->out[n];
        o = *m;
        o.midiData[1] = (char)(m->midiData[1] + 12);
        t->outList.events[n++] = reinterpret_cast<VstEvent*>(&o);
      }
      t->outList.numEvents = n;
      if (n && t->host) t->host(e, audioMasterProcessEvents, 0, 0, &t->outList, 0);
      return 1;
    }
  }
  return 0;
}

void YOV_VSTCALL processReplacing(AEffect* e, float**, float** outs, int32_t frames) {
  auto* t = self(e);
  for (int c = 0; c < e->numOutputs; ++c)
    for (int i = 0; i < frames; ++i) outs[c][i] = t->held ? 0.1f * float(c + 1) : 0.f;
}

void YOV_VSTCALL setParameter(AEffect*, int32_t, float) {}
float YOV_VSTCALL getParameter(AEffect*, int32_t) { return 0.f; }

}  // namespace

extern "C" __declspec(dllexport) AEffect* VSTPluginMain(HostCallback host) {
  auto* t = new TestPlugin();
  t->host = host;
  AEffect& e = t->fx;
  e.magic = kMagic;
  e.dispatcher = dispatcher;
  e.setParameter = setParameter;
  e.getParameter = getParameter;
  e.processReplacing = processReplacing;
  e.numInputs = 0;
  e.numOutputs = 6;
  e.flags = effFlagsCanReplacing | effFlagsIsSynth;
  e.uniqueID = 'YOVt';
  e.version = 1;
  e.object = t;
  return &e;
}
