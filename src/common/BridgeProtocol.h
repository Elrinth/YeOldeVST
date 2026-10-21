// Shared-memory protocol between the plugin (client, always 64-bit) and a bridge helper process
// (YeOldeVST-Host32/64.exe --bridge <name>) that hosts one VST2 plugin.
//
// Everything in shared memory uses fixed-size types and no pointers, so a 32-bit and a 64-bit
// process see the same layout.
//
// Channels:
//  * control: one request at a time from the client's main thread; the bridge's main thread
//    (which also runs the plugin's editor) answers. ctrlReq / ctrlResp events.
//  * audio: one block at a time from the client's audio thread; the bridge's audio thread
//    answers. audioReq / audioResp events, plus sequence numbers so a late answer is detected.
//  * notifications: plugin -> client messages (automation, editor resize...) in a ring buffer,
//    with a notify event.
#pragma once

#include <cstdint>

namespace yov::bridge {

constexpr uint32_t kMagic = 0x594F5642;  // 'YOVB'
constexpr uint32_t kVersion = 2;

constexpr uint32_t kCtrlDataSize = 1u << 20;  // larger payloads travel in a separate mapping
constexpr uint32_t kMaxChannels = 64;         // per direction
constexpr uint32_t kMaxFrames = 4096;         // per bridge block; bigger host blocks are split
constexpr uint32_t kMaxMidi = 512;
constexpr uint32_t kMaxSysex = 16;
constexpr uint32_t kSysexBytes = 64 * 1024;
constexpr uint32_t kMaxParamChanges = 512;
constexpr uint32_t kParamCache = 4096;
constexpr uint32_t kRingSize = 1024;

enum Op : uint32_t {
  OpHello = 1,
  OpLoad,             // data: path (utf-8); index: shell id. result 1/0, data: error
  OpRefreshInfo,      // re-reads info into Shared::info
  OpSetup,            // dvalue: sample rate, index: max frames
  OpResume,
  OpSuspend,
  OpParamName,        // index -> data
  OpParamDisplay,     // index -> data (display + label)
  OpGetProgram,       // -> result
  OpSetProgram,       // index
  OpProgramNames,     // -> data: names separated by '\0'
  OpGetChunk,         // index: 1 preset / 0 bank -> data (or big mapping)
  OpSetChunk,         // index: 1 preset / 0 bank, data (or big mapping)
  OpSetParameter,     // index, dvalue
  OpEditorRect,       // -> result packed (w << 16 | h)
  OpEditorOpen,       // value: parent HWND -> result packed size, 0 = failed
  OpEditorClose,
  OpRefreshParams,    // re-read all parameter values into the cache
  OpQuit,
  OpParamNames,       // index: count -> data: names separated by '\0'
};

#pragma pack(push, 8)

struct Info {
  char name[256];
  char vendor[256];
  char product[256];
  int32_t uniqueId;
  int32_t numInputs, numOutputs, numParams, numPrograms;
  int32_t initialDelay, tailSamples;
  int32_t isSynth, hasEditor, usesChunks;
};

struct Ctrl {
  uint32_t seq;
  uint32_t op;
  int32_t index;
  int32_t result;
  int64_t value;
  double dvalue;
  uint32_t dataSize;
  uint32_t bigSize;   // >0: payload is in the mapping named bigName instead of data
  char bigName[96];
  uint8_t data[kCtrlDataSize];
};

struct MidiEvent {
  int32_t deltaFrames;
  uint8_t bytes[4];
  int32_t noteLength, noteOffset;
  int8_t detune;
  uint8_t noteOffVelocity;
  uint8_t pad[2];
};

struct SysexRef {
  int32_t deltaFrames;
  uint32_t offset, size;
};

struct ParamChange {
  int32_t index;
  float value;
};

// Mirrors vst2::VstTimeInfo (all fixed-size fields).
struct TimeInfo {
  double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
  int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

struct Audio {
  uint32_t seq;      // written by client
  uint32_t doneSeq;  // written by bridge when the block is finished
  int32_t frames;
  int32_t ok;        // bridge: 1 if the plugin processed
  int32_t numParams;
  ParamChange params[kMaxParamChanges];
  int32_t numMidi;
  MidiEvent midi[kMaxMidi];
  int32_t numSysex;
  SysexRef sysex[kMaxSysex];
  uint8_t sysexData[kSysexBytes];
  TimeInfo time;
  // MIDI the plugin sent out during this block (bridge -> client)
  int32_t numOutMidi;
  MidiEvent outMidi[kMaxMidi];
  int32_t numOutSysex;
  SysexRef outSysex[kMaxSysex];
  uint8_t outSysexData[kSysexBytes];
  float in[kMaxChannels][kMaxFrames];
  float out[kMaxChannels][kMaxFrames];
};

enum NotifyType : int32_t { NAutomate = 1, NBeginEdit, NEndEdit, NSizeWindow, NIOChanged, NUpdateDisplay };

struct Notify {
  int32_t type;
  int32_t index;
  float value;
  int32_t w, h;
};

struct Shared {
  uint32_t magic;
  uint32_t version;
  uint32_t clientPid;
  uint32_t bridgePid;
  volatile int32_t ready;
  volatile int32_t dead;  // the hosted plugin faulted (bridge itself still alive)
  char deathReason[512];
  Info info;
  volatile int32_t ringLock;
  volatile uint32_t ringWrite;
  volatile uint32_t ringRead;
  Notify ring[kRingSize];
  float paramCache[kParamCache];
  Ctrl ctrl;
  Audio audio;
};

#pragma pack(pop)

// Kernel object names derived from the base name.
inline void objectName(char* out, size_t cap, const char* base, const char* suffix) {
  int n = 0;
  for (const char* s = base; *s && n + 1 < (int)cap; ++s) out[n++] = *s;
  for (const char* s = suffix; *s && n + 1 < (int)cap; ++s) out[n++] = *s;
  out[n] = 0;
}

}  // namespace yov::bridge
