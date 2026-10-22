#include "plugin/HostedPlugin.h"

#include "common/Util.h"

#include <algorithm>

namespace yov {

namespace {

using namespace vst2;

class InProcessPlugin final : public HostedPlugin {
 public:
  explicit InProcessPlugin(std::unique_ptr<Vst2Plugin> p) : p_(std::move(p)) { refreshInfo(); }

  bool sandboxed() const override { return false; }
  const HostedInfo& info() const override { return info_; }

  void refreshInfo() override {
    auto& p = *p_;
    info_.name = p.effectName();
    info_.vendor = p.vendorName();
    info_.product = p.productName();
    info_.uniqueId = p.uniqueId();
    info_.numInputs = std::clamp(p.numInputs(), 0, 64);
    info_.numOutputs = std::clamp(p.numOutputs(), 0, 64);
    info_.numParams = std::max(0, p.numParams());
    info_.numPrograms = std::max(0, p.numPrograms());
    info_.initialDelay = std::max(0, p.initialDelay());
    info_.isSynth = p.isSynth();
    info_.hasEditor = p.hasEditor();
    info_.usesChunks = p.usesChunks();
    info_.tailSamples = (int32_t)std::max<intptr_t>(0, p.dispatch(effGetTailSize));
  }

  bool dead() const override { return p_->dead(); }
  std::string deathReason() const override {
    if (!p_->dead()) return {};
    return p_->deathReason() + " (in " + p_->deathStage() + ")";
  }

  void setupProcessing(double sr, int32_t maxFrames) override {
    bool was = p_->resumed();
    if (was) p_->suspend();
    p_->setSampleRate(sr);
    p_->setBlockSize(maxFrames);
    if (was) p_->resume();
  }
  void resume() override { p_->resume(); }
  void suspend() override { p_->suspend(); }

  std::string paramName(int32_t i) override { return p_->dispatchString(effGetParamName, i); }
  std::string paramDisplay(int32_t i) override {
    auto d = p_->dispatchString(effGetParamDisplay, i);
    auto l = p_->dispatchString(effGetParamLabel, i);
    if (!l.empty()) d += " " + l;
    return d;
  }
  int32_t currentProgram() override { return (int32_t)p_->dispatch(effGetProgram); }
  void setProgram(int32_t i) override {
    p_->dispatch(effBeginSetProgram);
    p_->dispatch(effSetProgram, 0, i);
    p_->dispatch(effEndSetProgram);
  }
  std::vector<std::string> programNames() override {
    std::vector<std::string> names;
    int n = std::min(info_.numPrograms, 1024);
    for (int i = 0; i < n; ++i) {
      auto s = p_->dispatchString(effGetProgramNameIndexed, i);
      if (s.empty() && i == currentProgram()) s = p_->dispatchString(effGetProgramName);
      names.push_back(s);
    }
    return names;
  }
  bool getChunk(std::vector<uint8_t>& out, bool& isPreset) override {
    // Bank chunk like most hosts (KarmaFX hangs restoring a program chunk); program chunk only
    // for plugins that return no bank.
    isPreset = false;
    if (p_->getChunk(false, out) && !out.empty()) return true;
    isPreset = true;
    return p_->getChunk(true, out) && !out.empty();
  }
  bool setChunk(const std::vector<uint8_t>& d, bool isPreset) override {
    return p_->setChunk(isPreset, d.data(), d.size());
  }

  float getParameter(int32_t i) override { return p_->getParameter(i); }
  void setParameter(int32_t i, float v) override { p_->setParameter(i, v); }

  bool process(float** in, float** out, int32_t frames, const VstEvents* events, const VstTimeInfo& time) override {
    if (p_->dead()) return false;
    p_->timeInfo() = time;
    if (events && events->numEvents > 0) p_->processEvents(const_cast<VstEvents*>(events));
    return p_->process(in, info_.numInputs, out, info_.numOutputs, frames);
  }

  bool editorSize(int& w, int& h) override { return p_->editorGetRect(w, h); }
  bool editorOpen(void* parent) override { return p_->editorOpen(parent); }
  void editorClose() override { p_->editorClose(); }
  void editorIdle() override { p_->editorIdle(); }

 private:
  std::unique_ptr<Vst2Plugin> p_;
  HostedInfo info_;
};

}  // namespace

std::unique_ptr<HostedPlugin> loadInProcess(const std::string& path, int32_t shellId, HostedPlugin::Listener* l,
                                            std::string& error) {
  auto p = Vst2Plugin::load(path, shellId, l, error);
  if (!p) return nullptr;
  return std::make_unique<InProcessPlugin>(std::move(p));
}

}  // namespace yov
