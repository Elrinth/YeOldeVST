// One loaded VST 1.x/2.x plugin instance, hosted in this process.
//
// Every call into the plugin goes through an SEH guard. The first fault marks the instance
// dead: from then on no plugin code is called (not even effClose), the DLL is deliberately
// leaked, and dead() / deathReason() tell the UI what happened.
#pragma once

#include "common/Seh.h"
#include "vst2/aeffect.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace yov {

class Vst2Plugin {
 public:
  // Callbacks the plugin triggers through audioMaster. They can arrive on any thread.
  struct Listener {
    virtual ~Listener() = default;
    virtual void vstAutomate(int32_t index, float value) {}
    virtual void vstBeginEdit(int32_t index) {}
    virtual void vstEndEdit(int32_t index) {}
    virtual bool vstSizeWindow(int32_t width, int32_t height) { return false; }
    virtual void vstIOChanged() {}
    virtual void vstUpdateDisplay() {}
    virtual void vstMidiOut(const vst2::VstEvents* events) {}
  };

  // shellId: for shell plugins (WaveShell etc.) the sub-plugin to instantiate, 0 otherwise.
  static std::unique_ptr<Vst2Plugin> load(const std::string& path, int32_t shellId, Listener* listener,
                                          std::string& error);
  ~Vst2Plugin();

  Vst2Plugin(const Vst2Plugin&) = delete;
  Vst2Plugin& operator=(const Vst2Plugin&) = delete;

  vst2::AEffect* effect() const { return fx_; }
  const std::string& path() const { return path_; }
  int32_t shellId() const { return shellId_; }

  bool dead() const { return dead_.load(std::memory_order_acquire); }
  std::string deathReason() const;
  // Name of the last call that faulted ("effEditOpen", "process", ...).
  std::string deathStage() const;

  // Guarded plugin calls; all return 0 / do nothing once the plugin is dead.
  intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.f);
  std::string dispatchString(int32_t op, int32_t index = 0);
  float getParameter(int32_t index);
  void setParameter(int32_t index, float value);
  bool canDo(const char* what);

  // Info helpers (call after load)
  std::string effectName();
  std::string productName();
  std::string vendorName();
  int32_t vendorVersion();
  int32_t vstVersion();
  int32_t category();
  bool isSynth() const;
  bool hasEditor() const;
  bool usesChunks() const;
  bool canReplacing() const;
  int32_t numInputs() const;
  int32_t numOutputs() const;
  int32_t numParams() const;
  int32_t numPrograms() const;
  int32_t uniqueId() const;
  int32_t initialDelay() const;

  // Lifecycle. All main thread.
  void setSampleRate(double sr);
  void setBlockSize(int32_t maxFrames);
  void resume();   // effMainsChanged(1) + effStartProcess
  void suspend();  // effStopProcess + effMainsChanged(0)
  bool resumed() const { return resumed_; }

  // Audio thread. Buffers must have numInputs() / numOutputs() channels of `frames` samples.
  // Uses processReplacing when available, otherwise the VST 1.x accumulating process().
  bool process(float** inputs, float** outputs, int32_t frames);
  // Audio thread, before process(). The event list must stay valid during process().
  bool processEvents(vst2::VstEvents* events);
  // Audio thread: transport info the plugin reads through audioMasterGetTime.
  vst2::VstTimeInfo& timeInfo() { return time_; }

  // State
  bool getChunk(bool isPreset, std::vector<uint8_t>& out);
  bool setChunk(bool isPreset, const void* data, size_t size);

  // Editor (main thread)
  bool editorGetRect(int& width, int& height);
  bool editorOpen(void* parentWindow);
  void editorClose();
  void editorIdle();
  bool editorIsOpen() const { return editorOpen_; }

  double sampleRate() const { return sampleRate_; }
  int32_t blockSize() const { return blockSize_; }

 private:
  Vst2Plugin() = default;
  static intptr_t YOV_VSTCALL hostCallback(vst2::AEffect* fx, int32_t opcode, int32_t index, intptr_t value,
                                           void* ptr, float opt);
  intptr_t onHostCallback(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
  void markDead(const char* stage, const SehFault& f);

  template <class F>
  bool guarded(const char* stage, F&& f);

  std::string path_;
  std::string dir_;
  int32_t shellId_ = 0;
  void* module_ = nullptr;
  vst2::AEffect* fx_ = nullptr;
  Listener* listener_ = nullptr;

  std::atomic<bool> dead_{false};
  mutable std::mutex deathMutex_;
  std::string deathReason_;
  std::string deathStage_;

  double sampleRate_ = 44100.0;
  int32_t blockSize_ = 512;
  bool resumed_ = false;
  bool editorOpen_ = false;
  std::atomic<uint64_t> audioThreadId_{0};
  vst2::VstTimeInfo time_{};
  std::vector<std::shared_ptr<std::vector<uint8_t>>> chunkKeepAlive_;
};

}  // namespace yov
