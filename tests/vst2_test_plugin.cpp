// Tiny VST2 test plugin for YeOldeVST's own tests (built as 32-bit and 64-bit DLLs on Windows,
// as a .so on Linux).
//
// - 6 outputs; output c carries the constant level 0.1 * (c + 1) while a note is held, so a
//   test can check every channel reaches the right DAW port.
// - Echoes every incoming note on/off back to the host through audioMasterProcessEvents,
//   transposed up an octave, at the same sample offset (tests MIDI output).
// - Linux: a small X11 editor (a brass panel showing the held-note count), so editor embedding
//   can be tested in-process and through the bridge.
#include "vst2/aeffect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <X11/Xlib.h>
#define YOV_TEST_EDITOR 1
#endif

#if defined(_WIN32)
#define YOV_EXPORT extern "C" __declspec(dllexport)
#else
#define YOV_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace vst2;

namespace {

constexpr int kEditorW = 320, kEditorH = 200;

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
#if defined(YOV_TEST_EDITOR)
  Display* dpy = nullptr;
  Window win = 0;
  int shownHeld = -1;
#endif
};

TestPlugin* self(AEffect* e) { return static_cast<TestPlugin*>(e->object); }

#if defined(YOV_TEST_EDITOR)
void editorDraw(TestPlugin* t) {
  if (!t->dpy || !t->win) return;
  GC gc = XCreateGC(t->dpy, t->win, 0, nullptr);
  XSetForeground(t->dpy, gc, 0xD9A441);
  XFillRectangle(t->dpy, t->win, gc, 0, 0, kEditorW, kEditorH);
  XSetForeground(t->dpy, gc, 0x1C1D21);
  XFillRectangle(t->dpy, t->win, gc, 10, 10, kEditorW - 20, 40);
  XSetForeground(t->dpy, gc, 0xE3E5E8);
  char text[64];
  snprintf(text, sizeof text, "YOV Test editor - notes held: %d", t->held);
  XDrawString(t->dpy, t->win, gc, 20, 35, text, (int)strlen(text));
  XFreeGC(t->dpy, gc);
  XFlush(t->dpy);
  t->shownHeld = t->held;
}

bool editorOpen(TestPlugin* t, void* parent) {
  if (t->win) return false;
  t->dpy = XOpenDisplay(nullptr);
  if (!t->dpy) return false;
  XSetWindowAttributes a{};
  a.event_mask = ExposureMask | StructureNotifyMask;
  a.background_pixel = 0xD9A441;
  t->win = XCreateWindow(t->dpy, (Window)(uintptr_t)parent, 0, 0, kEditorW, kEditorH, 0, CopyFromParent, InputOutput,
                         CopyFromParent, CWEventMask | CWBackPixel, &a);
  XMapWindow(t->dpy, t->win);
  XSync(t->dpy, False);
  editorDraw(t);
  return true;
}

void editorIdle(TestPlugin* t) {
  if (!t->dpy) return;
  bool redraw = t->shownHeld != t->held;
  while (XPending(t->dpy)) {
    XEvent ev;
    XNextEvent(t->dpy, &ev);
    if (ev.type == Expose) redraw = true;
  }
  if (redraw) editorDraw(t);
}

void editorClose(TestPlugin* t) {
  if (t->dpy) {
    if (t->win) XDestroyWindow(t->dpy, t->win);
    XCloseDisplay(t->dpy);
  }
  t->dpy = nullptr;
  t->win = 0;
}
#endif

intptr_t YOV_VSTCALL dispatcher(AEffect* e, int32_t op, int32_t, intptr_t, void* ptr, float) {
  auto* t = self(e);
  switch (op) {
    case effClose:
#if defined(YOV_TEST_EDITOR)
      editorClose(t);
#endif
      delete t;
      return 1;
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
#if defined(YOV_TEST_EDITOR)
    case effEditGetRect: {
      static ERect r{0, 0, kEditorH, kEditorW};
      if (ptr) *(ERect**)ptr = &r;
      return 1;
    }
    case effEditOpen: return editorOpen(t, ptr) ? 1 : 0;
    case effEditIdle: editorIdle(t); return 1;
    case effEditClose: editorClose(t); return 1;
#endif
  }
  return 0;
}

void YOV_VSTCALL processReplacing(AEffect* e, float**, float** outs, int32_t frames) {
  auto* t = self(e);
  for (int c = 0; c < e->numOutputs; ++c)
    for (int i = 0; i < frames; ++i) outs[c][i] = t->held ? 0.1f * float(c + 1) : 0.f;
}

// Like some old plugins, reports every change straight back to the host (an "echo"), and keeps
// the value so a test can read it back.
float gParams[4] = {};
void YOV_VSTCALL setParameter(AEffect* e, int32_t i, float v) {
  if (i < 0 || i >= 4) return;
  gParams[i] = v;
  if (auto* t = self(e); t->host) t->host(e, audioMasterAutomate, i, 0, nullptr, v);
}
float YOV_VSTCALL getParameter(AEffect*, int32_t i) { return i >= 0 && i < 4 ? gParams[i] : 0.f; }

}  // namespace

YOV_EXPORT AEffect* VSTPluginMain(HostCallback host) {
  auto* t = new TestPlugin();
  t->host = host;
  AEffect& e = t->fx;
  e.magic = kMagic;
  e.dispatcher = dispatcher;
  e.setParameter = setParameter;
  e.getParameter = getParameter;
  e.processReplacing = processReplacing;
  e.numInputs = 0;
  // YOV_TEST_OUTPUTS overrides the output count (tests for plugins with very many outputs).
  e.numOutputs = 6;
  if (const char* n = getenv("YOV_TEST_OUTPUTS")) e.numOutputs = atoi(n) > 0 ? atoi(n) : 6;
  e.numParams = 4;
  e.flags = effFlagsCanReplacing | effFlagsIsSynth;
#if defined(YOV_TEST_EDITOR)
  e.flags |= effFlagsHasEditor;
#endif
  e.uniqueID = 'YOVt';
  e.version = 1;
  e.object = t;
  return &e;
}
