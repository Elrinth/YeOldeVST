// What the CLAP plugin talks to: one hosted VST2 plugin, either loaded into this process or
// running in a helper process (the bridge, used for 32-bit plugins and for "sandboxed" mode).
#pragma once

#include "host/Vst2Plugin.h"
#include "vst2/aeffect.h"

#include <memory>
#include <string>
#include <vector>

namespace yov {

struct HostedInfo {
  std::string name, vendor, product;
  int32_t uniqueId = 0;
  int32_t numInputs = 0, numOutputs = 0, numParams = 0, numPrograms = 0;
  int32_t initialDelay = 0;
  int32_t tailSamples = 0;
  bool isSynth = false, hasEditor = false, usesChunks = false;
};

class HostedPlugin {
 public:
  using Listener = Vst2Plugin::Listener;
  virtual ~HostedPlugin() = default;

  virtual bool sandboxed() const = 0;
  virtual const HostedInfo& info() const = 0;
  // Re-read numbers that can change at runtime (initialDelay after audioMasterIOChanged).
  virtual void refreshInfo() = 0;

  virtual bool dead() const = 0;
  virtual std::string deathReason() const = 0;

  // Main thread
  virtual void setupProcessing(double sampleRate, int32_t maxFrames) = 0;
  virtual void resume() = 0;
  virtual void suspend() = 0;
  virtual std::string paramName(int32_t index) = 0;
  virtual std::vector<std::string> paramNames(int32_t count) {
    std::vector<std::string> v;
    for (int32_t i = 0; i < count; ++i) v.push_back(paramName(i));
    return v;
  }
  virtual std::string paramDisplay(int32_t index) = 0;  // value text incl. label
  virtual int32_t currentProgram() = 0;
  virtual void setProgram(int32_t index) = 0;
  virtual std::vector<std::string> programNames() = 0;
  // Program ("preset") chunk preferred, bank chunk as fallback; isPreset tells which one.
  virtual bool getChunk(std::vector<uint8_t>& out, bool& isPreset) = 0;
  virtual bool setChunk(const std::vector<uint8_t>& data, bool isPreset) = 0;

  // Any thread
  virtual float getParameter(int32_t index) = 0;
  virtual void setParameter(int32_t index, float value) = 0;

  // Audio thread. in/out hold info().numInputs / numOutputs channels (at least one each).
  // Returns false if the plugin produced nothing (dead / busy): caller outputs silence.
  virtual bool process(float** in, float** out, int32_t frames, const vst2::VstEvents* events,
                       const vst2::VstTimeInfo& time) = 0;

  // Editor (main thread)
  virtual bool editorSize(int& width, int& height) = 0;
  virtual bool editorOpen(void* parentHwnd) = 0;
  virtual void editorClose() = 0;
  virtual void editorIdle() = 0;
};

// Loads a plugin into this process.
std::unique_ptr<HostedPlugin> loadInProcess(const std::string& path, int32_t shellId, HostedPlugin::Listener* l,
                                            std::string& error);
// Loads a plugin in a helper process (x86 helper for 32-bit DLLs, x64 for sandboxed 64-bit).
std::unique_ptr<HostedPlugin> loadBridged(const std::string& path, int32_t shellId, bool is32bit,
                                          HostedPlugin::Listener* l, std::string& error);

}  // namespace yov
