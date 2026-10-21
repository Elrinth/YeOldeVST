#include "host/Vst2Plugin.h"

#include "common/Util.h"

#include <windows.h>

#include <float.h>
#include <xmmintrin.h>

#include <cstring>

using namespace vst2;

namespace yov {

namespace {

// Set while a plugin's entry point runs: callbacks then arrive before we could tag the AEffect.
thread_local Vst2Plugin* tlsLoading = nullptr;
thread_local int32_t tlsLoadingShellId = 0;

// Plugins often leave the FPU in odd states (unmasked exceptions, denormals on, x87 precision
// changed). Save and restore around every audio call, and run them with denormals flushed.
struct FpuGuard {
  unsigned int mxcsr;
#if defined(_M_IX86)
  unsigned int x87;
#endif
  FpuGuard() {
    mxcsr = _mm_getcsr();
    _mm_setcsr((mxcsr | 0x8040 /*FTZ|DAZ*/ | 0x1F80 /*mask all*/));
#if defined(_M_IX86)
    x87 = _control87(0, 0);
#endif
  }
  ~FpuGuard() {
    _mm_setcsr(mxcsr);
#if defined(_M_IX86)
    _control87(x87, 0xFFFFFFFF);
#endif
  }
};

// ---- calling into the plugin ----
// MSVC's 32-bit code only keeps the stack 4-byte aligned, but plugins built with GCC or Intel's
// compiler assume 16-byte alignment at every call and crash in their SSE code otherwise (32-bit
// Sylenth1 is one). On x86 every call into a plugin goes through a thunk that aligns the stack.
#if defined(_M_IX86)
__declspec(noinline) intptr_t callDispatcher(DispatcherProc f, AEffect* e, int32_t op, int32_t idx, intptr_t val,
                                             void* aPtr, float aOpt) {
  intptr_t r;
  __asm {
    mov eax, esp
    and esp, 0xFFFFFFF0
    sub esp, 4  // 4 + saved esp 4 + 6 args 24 = 32
    push eax
    push aOpt
    push aPtr
    push val
    push idx
    push op
    push e
    call f
    add esp, 24
    pop esp
    mov r, eax
  }
  return r;
}
__declspec(noinline) void callProcess(ProcessProc f, AEffect* e, float** aIn, float** aOut, int32_t aN) {
  __asm {
    mov eax, esp
    and esp, 0xFFFFFFF0
    sub esp, 12  // 12 + 4 + 16 = 32
    push eax
    push aN
    push aOut
    push aIn
    push e
    call f
    add esp, 16
    pop esp
  }
}
__declspec(noinline) void callSetParameter(SetParameterProc f, AEffect* e, int32_t idx, float v) {
  __asm {
    mov eax, esp
    and esp, 0xFFFFFFF0
    push eax  // 4 + 12 = 16
    push v
    push idx
    push e
    call f
    add esp, 12
    pop esp
  }
}
__declspec(noinline) float callGetParameter(GetParameterProc f, AEffect* e, int32_t idx) {
  float r;
  __asm {
    mov eax, esp
    and esp, 0xFFFFFFF0
    sub esp, 4  // 4 + 4 + 8 = 16
    push eax
    push idx
    push e
    call f
    add esp, 8
    pop esp
    fstp r
  }
  return r;
}
__declspec(noinline) AEffect* callEntry(EntryProc f, HostCallback cb) {
  AEffect* r;
  __asm {
    mov eax, esp
    and esp, 0xFFFFFFF0
    sub esp, 8  // 8 + 4 + 4 = 16
    push eax
    push cb
    call f
    add esp, 4
    pop esp
    mov r, eax
  }
  return r;
}
#else
inline intptr_t callDispatcher(DispatcherProc f, AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr,
                               float opt) {
  return f(e, op, idx, val, ptr, opt);
}
inline void callProcess(ProcessProc f, AEffect* e, float** in, float** out, int32_t n) { f(e, in, out, n); }
inline void callSetParameter(SetParameterProc f, AEffect* e, int32_t idx, float v) { f(e, idx, v); }
inline float callGetParameter(GetParameterProc f, AEffect* e, int32_t idx) { return f(e, idx); }
inline AEffect* callEntry(EntryProc f, HostCallback cb) { return f(cb); }
#endif

std::string lastErrorText(DWORD err) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 err, 0, (LPWSTR)&msg, 0, nullptr);
  std::string s = msg ? trim(narrow(msg)) : "error " + std::to_string(err);
  LocalFree(msg);
  return s;
}

// Copy a C string a plugin wrote into an over-sized buffer, stopping at the terminator.
std::string fromPluginString(const char* buf, size_t cap) {
  size_t n = strnlen(buf, cap);
  std::string s(buf, n);
  // Old plugins return ANSI (Windows-1252 etc.); convert so the UI shows them right.
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) ascii = false;
  if (!ascii) {
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (wn <= 0) {
      wn = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
      std::wstring w(wn, L'\0');
      MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), wn);
      s = narrow(w);
    }
  }
  return trim(s);
}

}  // namespace

template <class F>
bool Vst2Plugin::guarded(const char* stage, F&& f) {
  if (dead()) return false;
  SehFault fault;
  if (sehGuard(f, &fault)) return true;
  markDead(stage, fault);
  return false;
}

void Vst2Plugin::markDead(const char* stage, const SehFault& f) {
  {
    std::lock_guard<std::mutex> lk(deathMutex_);
    if (dead_.load()) return;
    deathStage_ = stage;
    deathReason_ = f.describe();
  }
  dead_.store(true, std::memory_order_release);
  logLine("plugin '%s' crashed in %s: %s", path_.c_str(), stage, f.describe().c_str());
}

std::string Vst2Plugin::deathReason() const {
  std::lock_guard<std::mutex> lk(deathMutex_);
  return deathReason_;
}

std::string Vst2Plugin::deathStage() const {
  std::lock_guard<std::mutex> lk(deathMutex_);
  return deathStage_;
}

std::unique_ptr<Vst2Plugin> Vst2Plugin::load(const std::string& path, int32_t shellId, Listener* listener,
                                             std::string& error) {
  std::unique_ptr<Vst2Plugin> p(new Vst2Plugin());
  p->path_ = path;
  // audioMasterGetDirectory hands out a char*: old plugins expect the ANSI code page. For
  // folders the code page can't spell (Cyrillic on a Western system...) use the 8.3 short name.
  p->dir_ = ansiPath(dirOf(path));
  p->shellId_ = shellId;
  p->listener_ = listener;
  p->time_.sampleRate = p->sampleRate_;
  p->time_.tempo = 120.0;
  p->time_.timeSigNumerator = 4;
  p->time_.timeSigDenominator = 4;
  p->time_.flags = kVstTempoValid | kVstTimeSigValid | kVstPpqPosValid | kVstBarsValid;

  // Many old plugins find their own files (and DLLs they load at runtime) relative to the
  // current directory, as classic hosts set it to the plugin's folder while loading. Do the same
  // for the duration of the load, then put the host's directory back.
  struct CwdScope {
    std::unique_lock<std::mutex> lk;
    std::wstring old;
    explicit CwdScope(const std::string& dir) : lk(cwdMutex()) {
      wchar_t buf[4096];
      DWORD n = GetCurrentDirectoryW(4096, buf);
      if (n > 0 && n < 4096) old.assign(buf, n);
      if (!dir.empty()) SetCurrentDirectoryW(widen(dir).c_str());
    }
    ~CwdScope() {
      if (!old.empty()) SetCurrentDirectoryW(old.c_str());
    }
    static std::mutex& cwdMutex() {
      static std::mutex m;
      return m;
    }
  } cwd(dirOf(path));

  // Don't let a missing dependency pop up a system error box.
  UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  HMODULE mod = nullptr;
  SehFault fault;
  bool ok = sehGuard([&] { mod = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); },
                     &fault);
  DWORD err = GetLastError();
  SetErrorMode(oldMode);
  if (!ok) {
    error = "crashed while loading DLL: " + fault.describe();
    p->dead_ = true;  // leak whatever got mapped
    p.release();
    return nullptr;
  }
  if (!mod) {
    if (err == ERROR_MOD_NOT_FOUND)
      error = "a DLL this plugin needs is missing (" + lastErrorText(err) + ")";
    else if (err == ERROR_BAD_EXE_FORMAT)
      error = "wrong architecture (32-bit vs 64-bit)";
    else
      error = "LoadLibrary failed: " + lastErrorText(err);
    return nullptr;
  }
  p->module_ = mod;

  EntryProc entry = (EntryProc)GetProcAddress(mod, "VSTPluginMain");
  if (!entry) entry = (EntryProc)GetProcAddress(mod, "main");
  if (!entry) entry = (EntryProc)GetProcAddress(mod, "main_plugin");
  if (!entry) entry = (EntryProc)GetProcAddress(mod, "_main");
  if (!entry) {
    error = "no VST2 entry point (VSTPluginMain/main)";
    FreeLibrary(mod);
    p->module_ = nullptr;
    return nullptr;
  }

  AEffect* fx = nullptr;
  tlsLoading = p.get();
  tlsLoadingShellId = shellId;
  ok = sehGuard([&] { fx = callEntry(entry, &Vst2Plugin::hostCallback); }, &fault);
  tlsLoading = nullptr;
  tlsLoadingShellId = 0;
  if (!ok) {
    error = "crashed in entry point: " + fault.describe();
    p->dead_ = true;
    p.release();  // never touch it again; leak the module
    return nullptr;
  }
  if (!fx) {
    error = "entry point returned no plugin (it refused to start, maybe a demo/licence check)";
    FreeLibrary(mod);
    p->module_ = nullptr;
    return nullptr;
  }
  if (fx->magic != kMagic) {
    error = "entry point returned an invalid plugin (bad magic)";
    FreeLibrary(mod);
    p->module_ = nullptr;
    return nullptr;
  }
  fx->resvd1 = reinterpret_cast<intptr_t>(p.get());
  p->fx_ = fx;

  tlsLoading = p.get();
  tlsLoadingShellId = shellId;
  p->dispatch(effOpen);
  tlsLoading = nullptr;
  tlsLoadingShellId = 0;
  if (p->dead()) {
    error = "crashed in effOpen: " + p->deathReason();
    p.release();
    return nullptr;
  }
  return p;
}

Vst2Plugin::~Vst2Plugin() {
  if (dead()) return;  // never call into a crashed plugin; leak it
  if (fx_) {
    if (editorOpen_) editorClose();
    if (resumed_) suspend();
    dispatch(effClose);
    fx_ = nullptr;
  }
  if (module_ && !dead()) {
    SehFault f;
    HMODULE m = (HMODULE)module_;
    sehGuard([&] { FreeLibrary(m); }, &f);
  }
}

intptr_t Vst2Plugin::dispatch(int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {
  if (!fx_ || dead()) return 0;
  intptr_t r = 0;
  static const char* kNames[] = {"effOpen", "effClose", "effSetProgram", "effGetProgram", "effSetProgramName",
                                 "effGetProgramName", "effGetParamLabel", "effGetParamDisplay", "effGetParamName"};
  const char* stage = op < 9 ? kNames[op]
                      : op == effEditOpen     ? "effEditOpen"
                      : op == effEditClose    ? "effEditClose"
                      : op == effEditIdle     ? "effEditIdle"
                      : op == effMainsChanged ? "effMainsChanged"
                      : op == effGetChunk     ? "effGetChunk"
                      : op == effSetChunk     ? "effSetChunk"
                      : op == effProcessEvents ? "effProcessEvents"
                                              : "dispatcher";
  auto fx = fx_;
  guarded(stage, [&] { r = callDispatcher(fx->dispatcher, fx, op, index, value, ptr, opt); });
  return r;
}

std::string Vst2Plugin::dispatchString(int32_t op, int32_t index) {
  // Plugins routinely overflow the documented string sizes; give them plenty of room.
  char buf[1024];
  std::memset(buf, 0, sizeof buf);
  dispatch(op, index, 0, buf, 0.f);
  buf[sizeof buf - 1] = 0;
  return fromPluginString(buf, sizeof buf);
}

float Vst2Plugin::getParameter(int32_t index) {
  if (!fx_ || dead() || index < 0 || index >= fx_->numParams) return 0.f;
  float v = 0.f;
  auto fx = fx_;
  guarded("getParameter", [&] { v = callGetParameter(fx->getParameter, fx, index); });
  return v;
}

void Vst2Plugin::setParameter(int32_t index, float value) {
  if (!fx_ || dead() || index < 0 || index >= fx_->numParams) return;
  auto fx = fx_;
  guarded("setParameter", [&] { callSetParameter(fx->setParameter, fx, index, value); });
}

bool Vst2Plugin::canDo(const char* what) { return dispatch(effCanDo, 0, 0, (void*)what) > 0; }

std::string Vst2Plugin::effectName() { return dispatchString(effGetEffectName); }
std::string Vst2Plugin::productName() { return dispatchString(effGetProductString); }
std::string Vst2Plugin::vendorName() { return dispatchString(effGetVendorString); }
int32_t Vst2Plugin::vendorVersion() { return (int32_t)dispatch(effGetVendorVersion); }
int32_t Vst2Plugin::vstVersion() { return (int32_t)dispatch(effGetVstVersion); }
int32_t Vst2Plugin::category() { return (int32_t)dispatch(effGetPlugCategory); }
bool Vst2Plugin::isSynth() const { return fx_ && (fx_->flags & effFlagsIsSynth); }
bool Vst2Plugin::hasEditor() const { return fx_ && (fx_->flags & effFlagsHasEditor); }
bool Vst2Plugin::usesChunks() const { return fx_ && (fx_->flags & effFlagsProgramChunks); }
bool Vst2Plugin::canReplacing() const { return fx_ && (fx_->flags & effFlagsCanReplacing) && fx_->processReplacing; }
int32_t Vst2Plugin::numInputs() const { return fx_ ? fx_->numInputs : 0; }
int32_t Vst2Plugin::numOutputs() const { return fx_ ? fx_->numOutputs : 0; }
int32_t Vst2Plugin::numParams() const { return fx_ ? fx_->numParams : 0; }
int32_t Vst2Plugin::numPrograms() const { return fx_ ? fx_->numPrograms : 0; }
int32_t Vst2Plugin::uniqueId() const { return fx_ ? fx_->uniqueID : 0; }
int32_t Vst2Plugin::initialDelay() const { return fx_ ? fx_->initialDelay : 0; }

void Vst2Plugin::setSampleRate(double sr) {
  sampleRate_ = sr;
  time_.sampleRate = sr;
  dispatch(effSetSampleRate, 0, 0, nullptr, (float)sr);
}

void Vst2Plugin::setBlockSize(int32_t maxFrames) {
  blockSize_ = maxFrames;
  dispatch(effSetBlockSize, 0, maxFrames);
}

void Vst2Plugin::resume() {
  if (resumed_) return;
  dispatch(effMainsChanged, 0, 1);
  dispatch(effStartProcess);
  resumed_ = true;
}

void Vst2Plugin::suspend() {
  if (!resumed_) return;
  dispatch(effStopProcess);
  dispatch(effMainsChanged, 0, 0);
  resumed_ = false;
}

bool Vst2Plugin::process(float** inputs, float** outputs, int32_t frames) {
  if (!fx_ || dead()) return false;
  audioThreadId_.store(GetCurrentThreadId(), std::memory_order_relaxed);
  FpuGuard fpu;
  auto fx = fx_;
  if (canReplacing()) {
    return guarded("processReplacing", [&] { callProcess(fx->processReplacing, fx, inputs, outputs, frames); });
  }
  // VST 1.x: process() adds into the outputs.
  for (int32_t c = 0; c < fx->numOutputs; ++c) std::memset(outputs[c], 0, sizeof(float) * size_t(frames));
  if (!fx->process) return false;
  return guarded("process", [&] { callProcess(fx->process, fx, inputs, outputs, frames); });
}

bool Vst2Plugin::processEvents(VstEvents* events) {
  if (!fx_ || dead()) return false;
  audioThreadId_.store(GetCurrentThreadId(), std::memory_order_relaxed);
  FpuGuard fpu;
  dispatch(effProcessEvents, 0, 0, events);
  return !dead();
}

bool Vst2Plugin::getChunk(bool isPreset, std::vector<uint8_t>& out) {
  out.clear();
  void* data = nullptr;
  intptr_t size = dispatch(effGetChunk, isPreset ? 1 : 0, 0, &data);
  if (dead() || size <= 0 || !data) return false;
  if (size > (intptr_t)1 << 30) return false;  // nonsense size
  SehFault f;
  bool ok = sehGuard([&] { out.assign((uint8_t*)data, (uint8_t*)data + size); }, &f);
  if (!ok) {
    markDead("effGetChunk (copy)", f);
    out.clear();
  }
  return ok;
}

bool Vst2Plugin::setChunk(bool isPreset, const void* data, size_t size) {
  if (!data || size == 0) return false;
  // Some plugins (NI Massive, ...) keep reading the buffer on a background thread after
  // effSetChunk returns, so it must outlive the call. Keep it until the next setChunk / close.
  auto copy = std::make_shared<std::vector<uint8_t>>((const uint8_t*)data, (const uint8_t*)data + size);
  dispatch(effSetChunk, isPreset ? 1 : 0, (intptr_t)copy->size(), copy->data());
  chunkKeepAlive_.push_back(std::move(copy));
  if (chunkKeepAlive_.size() > 2) chunkKeepAlive_.erase(chunkKeepAlive_.begin());
  return !dead();
}

bool Vst2Plugin::editorGetRect(int& width, int& height) {
  width = height = 0;
  if (!hasEditor()) return false;
  ERect* r = nullptr;
  dispatch(effEditGetRect, 0, 0, &r);
  if (dead() || !r) return false;
  SehFault f;
  ERect copy{};
  if (!sehGuard([&] { copy = *r; }, &f)) return false;
  width = copy.right - copy.left;
  height = copy.bottom - copy.top;
  return width > 0 && height > 0;
}

bool Vst2Plugin::editorOpen(void* parentWindow) {
  if (!hasEditor() || editorOpen_) return false;
  // Several plugins only report a valid size after effEditGetRect has been called once before
  // opening; mimic what classic hosts did.
  int w, h;
  editorGetRect(w, h);
  dispatch(effEditOpen, 0, 0, parentWindow);
  if (dead()) return false;
  editorOpen_ = true;
  return true;
}

void Vst2Plugin::editorClose() {
  if (!editorOpen_) return;
  editorOpen_ = false;
  dispatch(effEditClose);
}

void Vst2Plugin::editorIdle() {
  if (editorOpen_) dispatch(effEditIdle);
}

intptr_t YOV_VSTCALL Vst2Plugin::hostCallback(AEffect* fx, int32_t opcode, int32_t index, intptr_t value, void* ptr,
                                              float opt) {
  Vst2Plugin* self = nullptr;
  if (fx && fx->magic == kMagic && fx->resvd1) self = reinterpret_cast<Vst2Plugin*>(fx->resvd1);
  if (!self) self = tlsLoading;

  if (opcode == audioMasterVersion) return 2400;
  if (opcode == audioMasterCurrentId) {
    if (tlsLoading) return tlsLoadingShellId;
    return self ? self->shellId_ : 0;
  }
  if (!self) {
    // Unknown instance: answer the static questions only.
    switch (opcode) {
      case audioMasterGetSampleRate: return 44100;
      case audioMasterGetBlockSize: return 512;
      case audioMasterGetVendorString:
        if (ptr) strcpy((char*)ptr, "YeOlde");
        return 1;
      case audioMasterGetProductString:
        if (ptr) strcpy((char*)ptr, "YeOldeVST");
        return 1;
      case audioMasterGetVendorVersion: return 1000;
      default: return 0;
    }
  }
  return self->onHostCallback(opcode, index, value, ptr, opt);
}

intptr_t Vst2Plugin::onHostCallback(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
  switch (opcode) {
    case audioMasterAutomate:
      if (listener_) listener_->vstAutomate(index, opt);
      return 0;
    case audioMasterIdle: return 1;
    case audioMasterWantMidi: return 1;
    case audioMasterGetTime: return reinterpret_cast<intptr_t>(&time_);
    case audioMasterProcessEvents:
      if (listener_ && ptr) listener_->vstMidiOut(static_cast<VstEvents*>(ptr));
      return 1;
    case audioMasterIOChanged:
      if (listener_) listener_->vstIOChanged();
      return 1;
    case audioMasterNeedIdle: return 1;
    case audioMasterSizeWindow: return (listener_ && listener_->vstSizeWindow(index, (int32_t)value)) ? 1 : 0;
    case audioMasterGetSampleRate: return (intptr_t)sampleRate_;
    case audioMasterGetBlockSize: return blockSize_;
    case audioMasterGetInputLatency: return 0;
    case audioMasterGetOutputLatency: return 0;
    case audioMasterGetCurrentProcessLevel:
      return GetCurrentThreadId() == audioThreadId_.load(std::memory_order_relaxed) ? kVstProcessLevelRealtime
                                                                                    : kVstProcessLevelUser;
    case audioMasterGetAutomationState: return 1;  // off
    case audioMasterGetVendorString:
      if (ptr) strcpy((char*)ptr, "YeOlde");
      return 1;
    case audioMasterGetProductString:
      if (ptr) strcpy((char*)ptr, "YeOldeVST");
      return 1;
    case audioMasterGetVendorVersion: return 1000;
    case audioMasterCanDo: {
      if (!ptr) return 0;
      static const char* kYes[] = {"sendVstEvents",     "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents",
                                   "receiveVstMidiEvent", "sizeWindow",     "startStopProcess", "supportShell",
                                   "shellCategory",     "supplyIdle"};
      for (auto s : kYes)
        if (std::strcmp((const char*)ptr, s) == 0) return 1;
      return 0;
    }
    case audioMasterGetLanguage: return 1;  // English
    case audioMasterGetDirectory: return reinterpret_cast<intptr_t>(dir_.c_str());
    case audioMasterUpdateDisplay:
      if (listener_) listener_->vstUpdateDisplay();
      return 1;
    case audioMasterBeginEdit:
      if (listener_) listener_->vstBeginEdit(index);
      return 1;
    case audioMasterEndEdit:
      if (listener_) listener_->vstEndEdit(index);
      return 1;
    default: return 0;
  }
}

}  // namespace yov
