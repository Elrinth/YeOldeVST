// YeOldeVST-Host64(.exe) / YeOldeVST-Host32.exe
//
// Helper process used by the plugin:
//   --probe <dll> [--shell-id N] [--editor]   test one plugin, report on stdout (scanner)
//   --bridge <name>                            host one plugin for an out-of-process instance
// and handy from a terminal:
//   --scan <folder> [<folder>...] [--db file.json] [--rescan] [--retry] [--no-editor]
//          [--parallel N] [--timeout seconds] [--visible]
//   --render <dll> <out.wav> [--shell-id N]    play a note through a plugin, write a WAV
#include "common/PluginDb.h"
#include "common/Util.h"
#include "host/Vst2Plugin.h"
#include "hostexe/Bridge.h"
#include "hostexe/Probe.h"
#include "scan/Scanner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace yov;

static int usage() {
  fprintf(stderr,
          "YeOldeVST helper\n"
          "  --scan <folder>... [--db file.json] [--rescan] [--retry] [--no-editor]\n"
          "         [--parallel N] [--timeout seconds] [--visible]\n"
          "  --probe <plugin.dll> [--shell-id N] [--editor]\n"
          "  --render <plugin.dll> <out.wav> [--shell-id N]\n");
  return 1;
}

static int cmdScan(const std::vector<std::string>& args) {
  Scanner::Options opt;
  PluginDb db;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--db" && i + 1 < args.size()) db.setFilePath(args[++i]);
    else if (a == "--rescan") opt.rescanAll = true;
    else if (a == "--retry") opt.retryFailed = true;
    else if (a == "--no-editor") opt.testEditors = false;
    else if (a == "--visible") opt.hiddenDesktop = false;
    else if (a == "--parallel" && i + 1 < args.size()) opt.parallel = std::atoi(args[++i].c_str());
    else if (a == "--timeout" && i + 1 < args.size()) opt.timeoutMs = std::atoi(args[++i].c_str()) * 1000;
    else opt.folders.push_back(a);
  }
  if (opt.folders.empty()) return usage();
  db.load();
  Scanner sc(db);
  sc.onResult = [](const PluginRecord& r) {
    printf("%-11s %-4s %-40s %s%s%s\n", statusName(r.status), r.arch.c_str(), r.displayName().c_str(),
           r.detail.c_str(), r.detail.empty() ? "" : "  ", r.path.c_str());
    fflush(stdout);
  };
  sc.start(opt);
  while (sc.running()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sc.wait();
  printf("%s\nDatabase: %s\n", sc.progress().message.c_str(), db.filePath().c_str());
  return 0;
}

static void writeWav(const std::string& path, const std::vector<float>& interleaved, int channels, int sr) {
  FILE* f = openFile(path, "wb");
  if (!f) return;
  uint32_t dataBytes = (uint32_t)(interleaved.size() * 4);
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f);
  u32(36 + dataBytes);
  fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(3);  // IEEE float
  u16((uint16_t)channels);
  u32((uint32_t)sr);
  u32((uint32_t)(sr * channels * 4));
  u16((uint16_t)(channels * 4));
  u16(32);
  fwrite("data", 1, 4, f);
  u32(dataBytes);
  fwrite(interleaved.data(), 4, interleaved.size(), f);
  fclose(f);
}

static int cmdRender(const std::string& dll, const std::string& out, int32_t shellId) {
  std::string err;
  auto p = Vst2Plugin::load(dll, shellId, nullptr, err);
  if (!p) {
    fprintf(stderr, "load failed: %s\n", err.c_str());
    return 2;
  }
  printf("%s by %s: %d in / %d out, %d params, synth=%d\n", p->effectName().c_str(), p->vendorName().c_str(),
         p->numInputs(), p->numOutputs(), p->numParams(), (int)p->isSynth());
  const int sr = 44100, block = 256, seconds = 3;
  p->setSampleRate(sr);
  p->setBlockSize(block);
  p->resume();
  int nin = std::clamp(p->numInputs(), 1, (int)Vst2Plugin::kChannelLimit);
  int nout = std::clamp(p->numOutputs(), 1, (int)Vst2Plugin::kChannelLimit);
  std::vector<std::vector<float>> in(nin, std::vector<float>(block)), o(nout, std::vector<float>(block));
  std::vector<float*> ip, op;
  for (auto& b : in) ip.push_back(b.data());
  for (auto& b : o) op.push_back(b.data());
  std::vector<float> wav;
  int blocks = sr * seconds / block;
  double phase = 0;
  for (int b = 0; b < blocks; ++b) {
    // Effects get a decaying 220 Hz saw burst every second, synths a note.
    for (int i = 0; i < block; ++i) {
      int t = (b * block + i) % sr;
      float env = t < sr / 4 ? 1.f - float(t) / (sr / 4) : 0.f;
      phase += 220.0 / sr;
      phase -= (int)phase;
      for (auto& ch : in) ch[i] = float(phase * 2 - 1) * 0.3f * env;
    }
    if (b == 1 || b == blocks / 2) {
      vst2::VstMidiEvent ev{};
      ev.type = vst2::kVstMidiType;
      ev.byteSize = sizeof ev;
      ev.midiData[0] = (char)(b == 1 ? 0x90 : 0x80);
      ev.midiData[1] = 57;
      ev.midiData[2] = 100;
      vst2::VstEvents evs{};
      evs.numEvents = 1;
      evs.events[0] = reinterpret_cast<vst2::VstEvent*>(&ev);
      p->processEvents(&evs);
    }
    p->timeInfo().samplePos = double(b) * block;
    p->process(ip.data(), (int32_t)ip.size(), op.data(), (int32_t)op.size(), block);
    if (p->dead()) {
      fprintf(stderr, "plugin crashed: %s (%s)\n", p->deathReason().c_str(), p->deathStage().c_str());
      return 3;
    }
    for (int i = 0; i < block; ++i) {
      wav.push_back(o[0][i]);
      wav.push_back(o[nout > 1 ? 1 : 0][i]);
    }
  }
  p->suspend();
  writeWav(out, wav, 2, sr);
  float peak = 0;
  for (float s : wav) peak = std::max(peak, std::fabs(s));
  printf("wrote %s, peak %.3f\n", out.c_str(), peak);
  return 0;
}

static int run(const std::vector<std::string>& args) {
  if (args.empty()) return usage();

  if (args[0] == "--probe" && args.size() >= 2) {
    ProbeOptions opt;
    opt.path = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--shell-id" && i + 1 < args.size()) opt.shellId = (int32_t)std::strtol(args[++i].c_str(), nullptr, 10);
      else if (args[i] == "--editor") opt.testEditor = true;
      else if (args[i] == "--editor-only") opt.editorOnly = true;
    }
    return runProbe(opt);
  }
  if (args[0] == "--bridge" && args.size() >= 2) {
    return runBridge(args[1]);
  }
  if (args[0] == "--scan") return cmdScan(std::vector<std::string>(args.begin() + 1, args.end()));
  if (args[0] == "--render" && args.size() >= 3) {
    int32_t shell = 0;
    if (args.size() >= 5 && args[3] == "--shell-id") shell = (int32_t)std::strtol(args[4].c_str(), nullptr, 10);
    return cmdRender(args[1], args[2], shell);
  }
  return usage();
}

#ifdef _WIN32
// Arguments as UTF-8 (plugin paths may be Cyrillic etc.).
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(narrow(argv[i]));
  return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
