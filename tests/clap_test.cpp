// Minimal CLAP host used to test YeOldeVST.clap without a DAW.
//
//   yov_clap_test <YeOldeVST.clap> render <instrument|effect> <vst2.dll> [out.wav]
//       loads the VST2 via a state blob, plays notes / audio, saves + restores state into a
//       second instance and checks both produce sound.
//   yov_clap_test <YeOldeVST.clap> gui <instrument|effect> <out.bmp> [vst2.dll] [seconds]
//       opens the plugin GUI in a window, pumps messages, saves a screenshot.
//
// Windows: Win32 window. Linux: X11 window, with the CLAP timer / fd support an X11 plugin GUI
// needs from its host. YOV_TEST_GENERIC=1 switches a loaded plugin to the generic editor.
#include <clap/clap.h>

#include "../src/plugin/clap-program-list.h"

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
static std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L' ');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}
static FILE* openOut(const std::string& path) { return _wfopen(wide(path).c_str(), L"wb"); }
#else
static FILE* openOut(const std::string& path) { return fopen(path.c_str(), "wb"); }
#endif

static uint64_t nowMs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::atomic<bool> gCallback{false};
static std::atomic<bool> gRestart{false};
static uint32_t gReqW = 0, gReqH = 0;

static bool hostResize(const clap_host_t*, uint32_t w, uint32_t h) {
  gReqW = w;
  gReqH = h;
  return true;
}
static void hostResizeHints(const clap_host_t*) {}
static bool hostReqShow(const clap_host_t*) { return true; }
static bool hostReqHide(const clap_host_t*) { return true; }
static void hostClosed(const clap_host_t*, bool) {}
static const clap_host_gui_t kHostGui{hostResizeHints, hostResize, hostReqShow, hostReqHide, hostClosed};

static void paramsRescan(const clap_host_t*, clap_param_rescan_flags) {}
static void paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
static void paramsFlush(const clap_host_t*) {}
static const clap_host_params_t kHostParams{paramsRescan, paramsClear, paramsFlush};

static void stateDirty(const clap_host_t*) {}
static const clap_host_state_t kHostState{stateDirty};

// ---- timer / fd support (what an X11 plugin GUI runs on) ----
struct Timer {
  clap_id id;
  uint32_t period;
  uint64_t next;
};
static std::vector<Timer> gTimers;
static clap_id gNextTimer = 1;
static bool timerRegister(const clap_host_t*, uint32_t period, clap_id* id) {
  *id = gNextTimer++;
  gTimers.push_back({*id, std::max<uint32_t>(period, 1), nowMs() + period});
  return true;
}
static bool timerUnregister(const clap_host_t*, clap_id id) {
  auto n = gTimers.size();
  gTimers.erase(std::remove_if(gTimers.begin(), gTimers.end(), [&](const Timer& t) { return t.id == id; }),
                gTimers.end());
  return gTimers.size() != n;
}
static const clap_host_timer_support_t kHostTimer{timerRegister, timerUnregister};

struct Fd {
  int fd;
  clap_posix_fd_flags_t flags;
};
static std::vector<Fd> gFds;
static bool fdRegister(const clap_host_t*, int fd, clap_posix_fd_flags_t flags) {
  gFds.push_back({fd, flags});
  return true;
}
static bool fdModify(const clap_host_t*, int fd, clap_posix_fd_flags_t flags) {
  for (auto& f : gFds)
    if (f.fd == fd) f.flags = flags;
  return true;
}
static bool fdUnregister(const clap_host_t*, int fd) {
  gFds.erase(std::remove_if(gFds.begin(), gFds.end(), [&](const Fd& f) { return f.fd == fd; }), gFds.end());
  return true;
}
static const clap_host_posix_fd_support_t kHostFd{fdRegister, fdModify, fdUnregister};

static const void* hostGetExt(const clap_host_t*, const char* id) {
  if (!strcmp(id, CLAP_EXT_GUI)) return &kHostGui;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
  if (!strcmp(id, CLAP_EXT_STATE)) return &kHostState;
#ifndef _WIN32
  if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kHostTimer;
  if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kHostFd;
#endif
  return nullptr;
}
static void hostRequestRestart(const clap_host_t*) { gRestart = true; }
static void hostRequestProcess(const clap_host_t*) {}
static void hostRequestCallback(const clap_host_t*) { gCallback = true; }

static clap_host_t kHost{CLAP_VERSION_INIT, nullptr, "yov_clap_test", "test", "", "1.0",
                         hostGetExt, hostRequestRestart, hostRequestProcess, hostRequestCallback};

// ---- streams ----
struct Buf {
  std::string data;
  size_t pos = 0;
};
static int64_t bufRead(const clap_istream_t* s, void* out, uint64_t n) {
  auto b = (Buf*)s->ctx;
  size_t k = (size_t)std::min<uint64_t>(n, b->data.size() - b->pos);
  memcpy(out, b->data.data() + b->pos, k);
  b->pos += k;
  return (int64_t)k;
}
static int64_t bufWrite(const clap_ostream_t* s, const void* in, uint64_t n) {
  ((Buf*)s->ctx)->data.append((const char*)in, (size_t)n);
  return (int64_t)n;
}

// ---- events ----
struct EventList {
  std::vector<clap_event_note_t> notes;
};
static uint32_t evSize(const clap_input_events_t* l) { return (uint32_t)((EventList*)l->ctx)->notes.size(); }
static const clap_event_header_t* evGet(const clap_input_events_t* l, uint32_t i) {
  return &((EventList*)l->ctx)->notes[i].header;
}
static int gMidiOut = 0;
static int gParamOut = 0;
static int gMidiOutFirstKey = -1;
static bool outPush(const clap_output_events_t*, const clap_event_header_t* h) {
  if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_MIDI) {
    auto m = reinterpret_cast<const clap_event_midi_t*>(h);
    if (gMidiOutFirstKey < 0) gMidiOutFirstKey = m->data[1];
    ++gMidiOut;
  }
  if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
    ++gParamOut;
    if (getenv("YOV_TEST_VERBOSE")) {
      auto pv = reinterpret_cast<const clap_event_param_value_t*>(h);
      printf("  plugin reported param %u = %.4f\n", pv->param_id, pv->value);
    }
  }
  return true;
}

static std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

// The state blob that loads a VST2. YOV_TEST_SANDBOXED=1 loads it in the bridge process (as
// "Load sandboxed" does); YOV_TEST_GENERIC=1 shows the generic editor.
static std::string testState(const char* path) {
  return std::string("{\"format\":\"YeOldeVST\",\"version\":1,\"path\":\"") + jsonEscape(path) + "\",\"shellId\":0" +
         (getenv("YOV_TEST_SANDBOXED") ? ",\"sandboxed\":true" : "") +
         (getenv("YOV_TEST_GENERIC") ? ",\"genericView\":true" : "") + "}";
}

struct Instance {
  const clap_plugin_t* p = nullptr;
  const clap_plugin_state_t* state = nullptr;
  const clap_plugin_params_t* params = nullptr;
  const clap_plugin_gui_t* gui = nullptr;
  const clap_plugin_audio_ports_t* ports = nullptr;
};

static Instance create(const clap_plugin_factory_t* f, const char* id) {
  Instance in;
  in.p = f->create_plugin(f, &kHost, id);
  if (!in.p || !in.p->init(in.p)) {
    printf("create/init failed\n");
    exit(2);
  }
  in.state = (const clap_plugin_state_t*)in.p->get_extension(in.p, CLAP_EXT_STATE);
  in.params = (const clap_plugin_params_t*)in.p->get_extension(in.p, CLAP_EXT_PARAMS);
  in.gui = (const clap_plugin_gui_t*)in.p->get_extension(in.p, CLAP_EXT_GUI);
  in.ports = (const clap_plugin_audio_ports_t*)in.p->get_extension(in.p, CLAP_EXT_AUDIO_PORTS);
  return in;
}

static void mainThreadPump(Instance& in) {
  if (gCallback.exchange(false)) in.p->on_main_thread(in.p);
}

static bool loadState(Instance& in, const std::string& s) {
  Buf b{s};
  clap_istream_t is{&b, bufRead};
  bool ok = in.state->load(in.p, &is);
  mainThreadPump(in);
  return ok;
}

static std::string saveState(Instance& in) {
  Buf b;
  clap_ostream_t os{&b, bufWrite};
  in.state->save(in.p, &os);
  return b.data;
}

// Plays 2 s: notes for an instrument, a decaying saw burst for an effect. Returns peak.
static float render(Instance& in, bool instrument, std::vector<float>* wav) {
  const double sr = 44100;
  const uint32_t block = 256;
  in.p->activate(in.p, sr, 32, block);
  in.p->start_processing(in.p);
  std::vector<float> inL(block), inR(block);
  float* inPtr[2] = {inL.data(), inR.data()};
  clap_audio_buffer_t ain{inPtr, nullptr, 2, 0, 0};
  uint32_t nports = in.ports ? in.ports->count(in.p, false) : 1;
  std::vector<std::vector<float>> outBufs(nports * 2, std::vector<float>(block));
  std::vector<float*> outPtrs;
  for (auto& b : outBufs) outPtrs.push_back(b.data());
  std::vector<clap_audio_buffer_t> aouts(nports);
  for (uint32_t i = 0; i < nports; ++i) aouts[i] = {&outPtrs[i * 2], nullptr, 2, 0, 0};
  std::vector<float> portPeak(nports, 0.f);
  float* outL = outPtrs[0];
  float* outR = outPtrs[1];
  gMidiOut = 0;
  gMidiOutFirstKey = -1;
  EventList evs;
  clap_input_events_t inEv{&evs, evSize, evGet};
  clap_output_events_t outEv{nullptr, outPush};
  clap_event_transport_t tr{};
  tr.header = {sizeof tr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
  tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING |
             CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
  tr.tempo = 120;
  tr.tsig_num = 4;
  tr.tsig_denom = 4;
  float peak = 0;
  int blocks = (int)(sr * 2 / block);
  double phase = 0;
  for (int b = 0; b < blocks; ++b) {
    evs.notes.clear();
    auto note = [&](uint16_t type, int16_t key) {
      clap_event_note_t n{};
      n.header = {sizeof n, 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
      n.note_id = -1;
      n.port_index = 0;
      n.channel = 0;
      n.key = key;
      n.velocity = 0.8;
      evs.notes.push_back(n);
    };
    if (b == 2) {
      note(CLAP_EVENT_NOTE_ON, 57);
      note(CLAP_EVENT_NOTE_ON, 64);
    }
    if (b == blocks / 2) {
      note(CLAP_EVENT_NOTE_OFF, 57);
      note(CLAP_EVENT_NOTE_OFF, 64);
    }
    for (uint32_t i = 0; i < block; ++i) {
      int t = (b * (int)block + (int)i) % 22050;
      float env = t < 4000 ? 1.f - t / 4000.f : 0.f;
      phase += 220.0 / sr;
      phase -= (int)phase;
      inL[i] = inR[i] = (float)(phase * 2 - 1) * 0.3f * env;
    }
    tr.song_pos_beats = (clap_beattime)(b * block / sr * 2.0 * CLAP_BEATTIME_FACTOR);
    clap_process_t pr{};
    pr.steady_time = (int64_t)b * block;
    pr.frames_count = block;
    pr.transport = &tr;
    pr.audio_inputs = instrument ? nullptr : &ain;
    pr.audio_inputs_count = instrument ? 0 : 1;
    pr.audio_outputs = aouts.data();
    pr.audio_outputs_count = nports;
    pr.in_events = &inEv;
    pr.out_events = &outEv;
    in.p->process(in.p, &pr);
    for (uint32_t port = 0; port < nports; ++port)
      for (uint32_t i = 0; i < block; ++i)
        if (b > 2)
          portPeak[port] = std::max(portPeak[port], std::max(std::fabs(outBufs[port * 2][i]), std::fabs(outBufs[port * 2 + 1][i])));
    for (uint32_t i = 0; i < block; ++i) {
      if (b > 2) peak = std::max(peak, std::max(std::fabs(outL[i]), std::fabs(outR[i])));
      if (wav) {
        wav->push_back(outL[i]);
        wav->push_back(outR[i]);
      }
    }
    mainThreadPump(in);
  }
  in.p->stop_processing(in.p);
  in.p->deactivate(in.p);
  if (nports > 1) {
    printf("ports: %u, peaks:", nports);
    for (float pp : portPeak) printf(" %.2f", pp);
    printf("\n");
  }
  printf("midi out events: %d (first key %d)\n", gMidiOut, gMidiOutFirstKey);
  return peak;
}

static void writeWav(const char* path, const std::vector<float>& d) {
  FILE* f = openOut(path);
  if (!f) return;
  uint32_t bytes = (uint32_t)d.size() * 4, v;
  uint16_t s;
  fwrite("RIFF", 1, 4, f);
  v = 36 + bytes;
  fwrite(&v, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  v = 16;
  fwrite(&v, 4, 1, f);
  s = 3;
  fwrite(&s, 2, 1, f);
  s = 2;
  fwrite(&s, 2, 1, f);
  v = 44100;
  fwrite(&v, 4, 1, f);
  v = 44100 * 8;
  fwrite(&v, 4, 1, f);
  s = 8;
  fwrite(&s, 2, 1, f);
  s = 32;
  fwrite(&s, 2, 1, f);
  fwrite("data", 1, 4, f);
  fwrite(&bytes, 4, 1, f);
  fwrite(d.data(), 4, d.size(), f);
  fclose(f);
}

// 24-bit bottom-up-less BMP from top-down BGR rows (stride padded to 4 bytes).
static void writeBmp(const char* path, int w, int h, const std::vector<uint8_t>& px) {
  int stride = ((w * 3 + 3) & ~3);
  uint8_t fh[14] = {'B', 'M'};
  uint8_t ih[40] = {};
  auto put32 = [](uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
  };
  put32(fh + 2, (uint32_t)(14 + 40 + px.size()));
  put32(fh + 10, 14 + 40);
  put32(ih, 40);
  put32(ih + 4, (uint32_t)w);
  put32(ih + 8, (uint32_t)-h);  // negative: top-down
  ih[12] = 1;
  ih[14] = 24;
  put32(ih + 20, (uint32_t)((size_t)stride * h));
  FILE* f = openOut(path);
  if (!f) return;
  fwrite(fh, 1, 14, f);
  fwrite(ih, 1, 40, f);
  fwrite(px.data(), 1, px.size(), f);
  fclose(f);
}

#ifndef _WIN32
// Linux: what the X server shows of the window (children included).
static void saveBmpX11(Display* d, Window w, const char* path) {
  XWindowAttributes a;
  XGetWindowAttributes(d, w, &a);
  XImage* img = XGetImage(d, w, 0, 0, (unsigned)a.width, (unsigned)a.height, AllPlanes, ZPixmap);
  if (!img) {
    printf("XGetImage failed\n");
    return;
  }
  int stride = ((a.width * 3 + 3) & ~3);
  std::vector<uint8_t> px((size_t)stride * a.height);
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      unsigned long p = XGetPixel(img, x, y);
      uint8_t* o = &px[(size_t)y * stride + x * 3];
      o[0] = (uint8_t)(p & 0xFF);
      o[1] = (uint8_t)((p >> 8) & 0xFF);
      o[2] = (uint8_t)((p >> 16) & 0xFF);
    }
  XDestroyImage(img);
  writeBmp(path, a.width, a.height, px);
}
#endif

#ifdef _WIN32
static void saveBmp(HWND w, const char* path) {
  RECT rc;
  GetClientRect(w, &rc);
  HDC dc = GetDC(w);
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
  SelectObject(mem, bmp);
  PrintWindow(w, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
  BITMAPINFOHEADER bi{sizeof bi, rc.right, -rc.bottom, 1, 24, BI_RGB};
  int stride = ((rc.right * 3 + 3) & ~3);
  std::vector<uint8_t> px((size_t)stride * rc.bottom);
  GetDIBits(mem, bmp, 0, rc.bottom, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
  BITMAPFILEHEADER fh{0x4D42, (DWORD)(sizeof fh + sizeof bi + px.size()), 0, 0, sizeof fh + sizeof bi};
  bi.biHeight = -rc.bottom;
  FILE* f = _wfopen(wide(path).c_str(), L"wb");
  fwrite(&fh, sizeof fh, 1, f);
  fwrite(&bi, sizeof bi, 1, f);
  fwrite(px.data(), 1, px.size(), f);
  fclose(f);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(w, dc);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}
#endif

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

#ifdef _WIN32
int main(int, char**) {
  // Arguments as UTF-8 (plugin paths may be Cyrillic etc.).
  int argc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  static std::vector<std::string> utf8Args;
  static std::vector<char*> argvVec;
  for (int i = 0; i < argc; ++i) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
    utf8Args.push_back(s);
  }
  for (auto& s : utf8Args) argvVec.push_back(s.data());
  char** argv = argvVec.data();
  if (argc < 4) {
    printf("usage: see source\n");
    return 1;
  }
  HMODULE mod = LoadLibraryW(wide(argv[1]).c_str());
  if (!mod) {
    printf("cannot load %s\n", argv[1]);
    return 2;
  }
  auto entry = (const clap_plugin_entry_t*)GetProcAddress(mod, "clap_entry");
#else
int main(int argc, char** argv) {
  if (argc < 4) {
    printf("usage: see source\n");
    return 1;
  }
  void* mod = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!mod) {
    printf("cannot load %s: %s\n", argv[1], dlerror());
    return 2;
  }
  auto entry = (const clap_plugin_entry_t*)dlsym(mod, "clap_entry");
#endif
  entry->init(argv[1]);
  auto f = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  std::string mode = argv[2];
  std::string flavour = argv[3];
  bool instrument = flavour != "effect";
  const char* id = flavour == "multi"      ? "com.thefirstboss.yeoldevst.instrument.multiout"
                   : flavour == "instrument" ? "com.thefirstboss.yeoldevst.instrument"
                                             : "com.thefirstboss.yeoldevst.effect";
  printf("factory has %u plugins\n", f->get_plugin_count(f));

  if (mode == "render" && argc >= 5) {
    std::string state = testState(argv[4]);
    Instance a = create(f, id);
    {
      // What a host that reads the list only once (right after creating us) gets.
      uint32_t n0 = a.params->count(a.p);
      int vis0 = 0;
      clap_param_info_t i0;
      for (uint32_t i = 0; i < n0; ++i)
        if (a.params->get_info(a.p, i, &i0) && !(i0.flags & CLAP_PARAM_IS_HIDDEN)) ++vis0;
      printf("before loading a VST: %d visible params\n", vis0);
    }
    loadState(a, state);
    uint32_t n = a.params->count(a.p);
    int visible = 0;
    clap_param_info_t info;
    std::string firstNames;
    for (uint32_t i = 0; i < n; ++i) {
      a.params->get_info(a.p, i, &info);
      if (!(info.flags & CLAP_PARAM_IS_HIDDEN)) {
        if (visible < 4) firstNames += std::string(info.name) + ", ";
        ++visible;
      }
    }
    printf("params: %u slots, %d visible (%s...)\n", n, visible, firstNames.c_str());
    if (visible > 0) {
      double v;
      char txt[128];
      a.params->get_value(a.p, 0, &v);
      a.params->value_to_text(a.p, 0, v, txt, sizeof txt);
      printf("param 0 = %.3f -> '%s'\n", v, txt);
    }
    if (visible > 3) {
      // Automate slot 3 the way a host does (an event through params.flush) and read it back.
      struct One {
        clap_event_param_value_t ev;
      } one{};
      one.ev.header = {sizeof one.ev, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
      one.ev.param_id = 3;
      one.ev.note_id = -1;
      one.ev.port_index = -1;
      one.ev.channel = -1;
      one.ev.key = -1;
      one.ev.value = 0.75;
      clap_input_events_t in{&one, [](const clap_input_events_t*) -> uint32_t { return 1; },
                             [](const clap_input_events_t* l, uint32_t) -> const clap_event_header_t* {
                               return &((One*)l->ctx)->ev.header;
                             }};
      clap_output_events_t out{nullptr, outPush};
      gParamOut = 0;
      a.params->flush(a.p, &in, &out);
      a.params->flush(a.p, nullptr, &out);  // anything the plugin reported back
      sleepMs(400);  // a bridged plugin's value cache refreshes a few times a second
      double v = -1;
      a.params->get_value(a.p, 3, &v);
      char txt[64] = {};
      a.params->value_to_text(a.p, 3, v, txt, sizeof txt);
      printf("automated param 3 to 0.75 -> plugin reports %.3f ('%s'), echoed back to host: %d\n", v, txt,
             gParamOut);
    }
    std::vector<float> wav;
    float p1 = render(a, instrument, &wav);
    printf("instance A peak: %.4f\n", p1);
    if (argc >= 6) writeWav(argv[5], wav);
    // The hosted plugin's programs (src/plugin/clap-program-list.h): listed, and the third one selected.
    bool programs_ok = false;
    if (auto pl = (const clap_plugin_program_list_t*)a.p->get_extension(a.p, CLAP_EXT_PROGRAM_LIST)) {
      const uint32_t np = pl->count(a.p);
      std::string names;
      for (uint32_t i = 0; i < np; ++i) {
        char nm[64] = {};
        pl->get_name(a.p, i, nm, sizeof nm);
        names += std::string(i ? ", " : "") + nm;
      }
      const bool selected = np > 2 && pl->select(a.p, 2) && pl->current(a.p) == 2;
      printf("programs: %u (%s), the third selected: %s\n", np, names.c_str(), selected ? "yes" : "no");
      programs_ok = np == 0 || selected;  // (a VST2 without programs lists none)
    } else {
      printf("programs: no program-list extension\n");
    }
    std::string saved = saveState(a);
    printf("state: %zu bytes: %.160s...\n", saved.size(), saved.c_str());
    a.p->destroy(a.p);

    Instance b = create(f, id);
    loadState(b, saved);
    float p2 = render(b, instrument, nullptr);
    printf("instance B (restored) peak: %.4f\n", p2);
    // The program selected before saving comes back (after a bank chunk too, which selects the first program).
    if (auto pl = (const clap_plugin_program_list_t*)b.p->get_extension(b.p, CLAP_EXT_PROGRAM_LIST); pl && pl->count(b.p) > 2) {
      const bool back = pl->current(b.p) == 2;
      printf("restored program: %d (%s)\n", pl->current(b.p), back ? "the one saved" : "NOT the one saved");
      programs_ok = programs_ok && back;
    }
    std::string saved2 = saveState(b);
    printf("restored state %s original\n", saved2 == saved ? "==" : "!=");
    b.p->destroy(b.p);
    entry->deinit();
    bool ok = p1 > 1e-4f && p2 > 1e-4f && programs_ok;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 3;
  }

#ifndef _WIN32
  if (mode == "gui" && argc >= 5) {
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
      printf("no X display\n");
      return 4;
    }
    Instance a = create(f, id);
    if (argc >= 6 && argv[5][0]) loadState(a, testState(argv[5]));
    a.p->activate(a.p, 44100, 32, 512);
    if (!a.gui->create(a.p, CLAP_WINDOW_API_X11, false)) {
      printf("gui create failed\n");
      return 5;
    }
    uint32_t w = 0, h = 0;
    a.gui->get_size(a.p, &w, &h);
    XSetWindowAttributes wa{};
    wa.background_pixel = BlackPixel(d, DefaultScreen(d));
    wa.event_mask = StructureNotifyMask;
    Window top = XCreateWindow(d, DefaultRootWindow(d), 40, 40, w, h, 0, CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask, &wa);
    XStoreName(d, top, "YeOldeVST test");
    XMapWindow(d, top);
    XSync(d, False);
    clap_window_t win{CLAP_WINDOW_API_X11};
    win.x11 = top;
    a.gui->set_parent(a.p, &win);
    a.gui->show(a.p);
    auto timerExt = (const clap_plugin_timer_support_t*)a.p->get_extension(a.p, CLAP_EXT_TIMER_SUPPORT);
    auto fdExt = (const clap_plugin_posix_fd_support_t*)a.p->get_extension(a.p, CLAP_EXT_POSIX_FD_SUPPORT);
    printf("gui: %ux%u, %zu timers, %zu fds\n", w, h, gTimers.size(), gFds.size());
    int seconds = argc >= 7 ? atoi(argv[6]) : 3;
    uint64_t end = nowMs() + (uint64_t)seconds * 1000;
    while (nowMs() < end) {
      // The host's event loop: plugin fds, plugin timers, our own window.
      std::vector<pollfd> pfds;
      for (auto& fd : gFds) pfds.push_back({fd.fd, (short)((fd.flags & CLAP_POSIX_FD_READ ? POLLIN : 0) |
                                                         (fd.flags & CLAP_POSIX_FD_WRITE ? POLLOUT : 0)),
                                            0});
      uint64_t now = nowMs();
      uint64_t next = now + 10;
      for (auto& t : gTimers) next = std::min(next, t.next);
      poll(pfds.data(), pfds.size(), (int)(next > now ? next - now : 0));
      for (auto& p : pfds)
        if (p.revents && fdExt)
          fdExt->on_fd(a.p, p.fd, (p.revents & POLLIN ? CLAP_POSIX_FD_READ : 0) |
                                      (p.revents & POLLOUT ? CLAP_POSIX_FD_WRITE : 0) |
                                      (p.revents & (POLLERR | POLLHUP) ? CLAP_POSIX_FD_ERROR : 0));
      now = nowMs();
      auto timers = gTimers;
      for (auto& t : timers)
        if (t.next <= now) {
          for (auto& real : gTimers)
            if (real.id == t.id) real.next = now + real.period;
          if (timerExt) timerExt->on_timer(a.p, t.id);
        }
      while (XPending(d)) {
        XEvent ev;
        XNextEvent(d, &ev);
      }
      mainThreadPump(a);
      if (gReqW) {
        XResizeWindow(d, top, gReqW, gReqH);
        XSync(d, False);
        a.gui->set_size(a.p, gReqW, gReqH);
        gReqW = gReqH = 0;
      }
    }
    XSync(d, False);
    saveBmpX11(d, top, argv[4]);
    a.gui->destroy(a.p);
    a.p->deactivate(a.p);
    a.p->destroy(a.p);
    XDestroyWindow(d, top);
    XCloseDisplay(d);
    entry->deinit();
    printf("saved %s\n", argv[4]);
    return 0;
  }
#else
  if (mode == "unload-scan") {
    // Start a scan from the GUI, then destroy the plugin and unload the DLL while it runs.
    // argv[3]: "deinit" = call entry->deinit() first (a well-behaved host), else just unload.
    bool callDeinit = std::string(argv[3]).find("deinit") == 0 || std::string(argv[3]) == "noscan-deinit";
    bool doScan = std::string(argv[3]).rfind("noscan", 0) != 0;
    bool withGui = std::string(argv[3]).find("nogui") == std::string::npos;
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"yovtest";
    RegisterClassW(&wc);
    Instance a = create(f, "com.thefirstboss.yeoldevst.instrument");
    a.gui->create(a.p, CLAP_WINDOW_API_WIN32, false);
    uint32_t w = 0, h = 0;
    a.gui->get_size(a.p, &w, &h);
    HWND top = CreateWindowW(L"yovtest", L"unload test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, (int)w + 16,
                             (int)h + 40, nullptr, nullptr, wc.hInstance, nullptr);
    clap_window_t win{CLAP_WINDOW_API_WIN32};
    win.win32 = top;
    a.gui->set_parent(a.p, &win);
    a.gui->show(a.p);
    MSG msg;
    auto pump = [&](DWORD ms) {
      DWORD end = GetTickCount() + ms;
      while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        mainThreadPump(a);
        Sleep(10);
      }
    };
    pump(500);
    struct Find {
      HWND found = nullptr;
      static BOOL CALLBACK cb(HWND hw, LPARAM p) {
        wchar_t t[64] = {}, c[64] = {};
        GetWindowTextW(hw, t, 64);
        GetClassNameW(hw, c, 64);
        if (!wcscmp(t, L"Scan") && !_wcsicmp(c, L"Button")) {
          ((Find*)p)->found = hw;
          return FALSE;
        }
        return TRUE;
      }
    } finder;
    EnumChildWindows(top, Find::cb, (LPARAM)&finder);
    if (!finder.found) {
      printf("no Scan button found\n");
      return 4;
    }
    if (doScan) SendMessageW(finder.found, BM_CLICK, 0, 0);
    pump(2500);  // the scan is now busy probing plugins
    printf("scan started; destroying the plugin and unloading (%s)\n", callDeinit ? "with deinit" : "no deinit");
    a.gui->destroy(a.p);
    a.p->destroy(a.p);
    DestroyWindow(top);
    if (callDeinit) entry->deinit();
    FreeLibrary((HMODULE)mod);
    pump(8000);  // if a scan thread kept running in unloaded code, the process dies here
    HMODULE still = GetModuleHandleW(wide(argv[1]).c_str());
    printf("still alive after 8 s; module %s\n", still ? "still loaded (scan finishing)" : "unloaded");
    printf("PASS\n");
    return 0;
  }

  if (mode == "gui" && argc >= 5) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"yovtest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    Instance a = create(f, id);
    if (argc >= 6) loadState(a, testState(argv[5]));
    a.p->activate(a.p, 44100, 32, 512);
    a.gui->create(a.p, CLAP_WINDOW_API_WIN32, false);
    uint32_t w = 0, h = 0;
    a.gui->get_size(a.p, &w, &h);
    RECT r{0, 0, (LONG)w, (LONG)h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND top = CreateWindowW(L"yovtest", L"YeOldeVST test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    clap_window_t win{CLAP_WINDOW_API_WIN32};
    win.win32 = top;
    a.gui->set_parent(a.p, &win);
    a.gui->show(a.p);
    int seconds = argc >= 7 ? atoi(argv[6]) : 3;
    DWORD end = GetTickCount() + seconds * 1000;
    MSG msg;
    while (GetTickCount() < end) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) end = 0;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      mainThreadPump(a);
      if (gReqW) {
        RECT rr{0, 0, (LONG)gReqW, (LONG)gReqH};
        AdjustWindowRect(&rr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(top, nullptr, 0, 0, rr.right - rr.left, rr.bottom - rr.top, SWP_NOMOVE | SWP_NOZORDER);
        a.gui->set_size(a.p, gReqW, gReqH);
        gReqW = gReqH = 0;
      }
      Sleep(10);
    }
    if (getenv("YOV_TEST_EXPAND_FIRST")) {
      // Select the first group row (it starts with the expand arrow) and press Right.
      HWND lv = FindWindowExW(FindWindowExW(FindWindowExW(top, nullptr, nullptr, nullptr), nullptr, nullptr, nullptr),
                              nullptr, L"SysListView32", nullptr);
      if (!lv) {
        // Search the whole tree.
        struct Find {
          static BOOL CALLBACK cb(HWND h, LPARAM p) {
            wchar_t c[64];
            GetClassNameW(h, c, 64);
            if (!wcscmp(c, L"SysListView32")) {
              *(HWND*)p = h;
              return FALSE;
            }
            return TRUE;
          }
        };
        EnumChildWindows(top, Find::cb, (LPARAM)&lv);
      }
      int count = lv ? (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0) : 0;
      for (int i = 0; i < count; ++i) {
        wchar_t text[256] = {};
        LVITEMW it{};
        it.iSubItem = 0;
        it.pszText = text;
        it.cchTextMax = 256;
        SendMessageW(lv, LVM_GETITEMTEXTW, i, (LPARAM)&it);
        if (text[0] == 0x25B8) {
          LVITEMW st{};
          st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
          st.state = LVIS_SELECTED | LVIS_FOCUSED;
          SendMessageW(lv, LVM_SETITEMSTATE, i, (LPARAM)&st);
          SetFocus(lv);
          SendMessageW(lv, WM_KEYDOWN, VK_RIGHT, 0);
          break;
        }
      }
      DWORD until = GetTickCount() + 500;
      while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
      }
    }
    saveBmp(top, argv[4]);
    a.gui->destroy(a.p);
    a.p->deactivate(a.p);
    a.p->destroy(a.p);
    DestroyWindow(top);
    entry->deinit();
    printf("saved %s\n", argv[4]);
    return 0;
  }
#endif
  return 1;
}
