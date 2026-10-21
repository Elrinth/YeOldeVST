// Clean-room description of the VST 1.x / 2.x binary interface.
//
// This header is NOT derived from the Steinberg VST2 SDK. It describes the ABI that existing
// plugin binaries expose, as documented publicly by many independent projects (VeSTige, FST,
// ...). Only what YeOldeVST needs is declared. Names are prefixed to avoid clashing with any
// real SDK that might be on an include path.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define YOV_VSTCALL __cdecl
#else
#define YOV_VSTCALL
#endif

namespace vst2 {

constexpr int32_t kMagic = ('V' << 24) | ('s' << 16) | ('t' << 8) | 'P';  // 'VstP'

struct AEffect;

using HostCallback = intptr_t(YOV_VSTCALL*)(AEffect* effect, int32_t opcode, int32_t index,
                                             intptr_t value, void* ptr, float opt);
using DispatcherProc = intptr_t(YOV_VSTCALL*)(AEffect* effect, int32_t opcode, int32_t index,
                                               intptr_t value, void* ptr, float opt);
using ProcessProc = void(YOV_VSTCALL*)(AEffect* effect, float** inputs, float** outputs,
                                        int32_t sampleFrames);
using ProcessDoubleProc = void(YOV_VSTCALL*)(AEffect* effect, double** inputs, double** outputs,
                                              int32_t sampleFrames);
using SetParameterProc = void(YOV_VSTCALL*)(AEffect* effect, int32_t index, float value);
using GetParameterProc = float(YOV_VSTCALL*)(AEffect* effect, int32_t index);
using EntryProc = AEffect*(YOV_VSTCALL*)(HostCallback host);

#pragma pack(push, 8)
struct AEffect {
  int32_t magic;
  DispatcherProc dispatcher;
  ProcessProc process;  // VST 1.x accumulating process (deprecated in 2.4)
  SetParameterProc setParameter;
  GetParameterProc getParameter;
  int32_t numPrograms;
  int32_t numParams;
  int32_t numInputs;
  int32_t numOutputs;
  int32_t flags;
  intptr_t resvd1;
  intptr_t resvd2;
  int32_t initialDelay;
  int32_t realQualities;
  int32_t offQualities;
  float ioRatio;
  void* object;
  void* user;
  int32_t uniqueID;
  int32_t version;
  ProcessProc processReplacing;
  ProcessDoubleProc processDoubleReplacing;
  char future[56];
};

struct ERect {
  int16_t top, left, bottom, right;
};

enum : int32_t { kVstMidiType = 1, kVstSysExType = 6 };

struct VstEvent {
  int32_t type;
  int32_t byteSize;
  int32_t deltaFrames;
  int32_t flags;
  char data[16];
};

struct VstMidiEvent {
  int32_t type;
  int32_t byteSize;
  int32_t deltaFrames;
  int32_t flags;
  int32_t noteLength;
  int32_t noteOffset;
  char midiData[4];
  char detune;
  char noteOffVelocity;
  char reserved1;
  char reserved2;
};

struct VstMidiSysexEvent {
  int32_t type;
  int32_t byteSize;
  int32_t deltaFrames;
  int32_t flags;
  int32_t dumpBytes;
  intptr_t resvd1;
  char* sysexDump;
  intptr_t resvd2;
};

// Variable length: events[numEvents]
struct VstEvents {
  int32_t numEvents;
  intptr_t reserved;
  VstEvent* events[2];
};

struct VstTimeInfo {
  double samplePos;
  double sampleRate;
  double nanoSeconds;
  double ppqPos;
  double tempo;
  double barStartPos;
  double cycleStartPos;
  double cycleEndPos;
  int32_t timeSigNumerator;
  int32_t timeSigDenominator;
  int32_t smpteOffset;
  int32_t smpteFrameRate;
  int32_t samplesToNextClock;
  int32_t flags;
};

struct VstPinProperties {
  char label[64];
  int32_t flags;
  int32_t arrangementType;
  char shortLabel[8];
  char future[48];
};
#pragma pack(pop)

enum : int32_t {
  kVstMidiEventIsRealtime = 1,
};

enum : int32_t {
  kVstTransportChanged = 1,
  kVstTransportPlaying = 1 << 1,
  kVstTransportCycleActive = 1 << 2,
  kVstTransportRecording = 1 << 3,
  kVstAutomationWriting = 1 << 6,
  kVstAutomationReading = 1 << 7,
  kVstNanosValid = 1 << 8,
  kVstPpqPosValid = 1 << 9,
  kVstTempoValid = 1 << 10,
  kVstBarsValid = 1 << 11,
  kVstCyclePosValid = 1 << 12,
  kVstTimeSigValid = 1 << 13,
  kVstSmpteValid = 1 << 14,
  kVstClockValid = 1 << 15,
};

// AEffect::flags
enum : int32_t {
  effFlagsHasEditor = 1 << 0,
  effFlagsCanReplacing = 1 << 4,
  effFlagsProgramChunks = 1 << 5,
  effFlagsIsSynth = 1 << 8,
  effFlagsNoSoundInStop = 1 << 9,
  effFlagsCanDoubleReplacing = 1 << 12,
};

// Plugin category (effGetPlugCategory)
enum : int32_t {
  kPlugCategUnknown = 0,
  kPlugCategEffect,
  kPlugCategSynth,
  kPlugCategAnalysis,
  kPlugCategMastering,
  kPlugCategSpacializer,
  kPlugCategRoomFx,
  kPlugSurroundFx,
  kPlugCategRestoration,
  kPlugCategOfflineProcess,
  kPlugCategShell,
  kPlugCategGenerator,
};

// Host -> plugin opcodes
enum : int32_t {
  effOpen = 0,
  effClose = 1,
  effSetProgram = 2,
  effGetProgram = 3,
  effSetProgramName = 4,
  effGetProgramName = 5,
  effGetParamLabel = 6,
  effGetParamDisplay = 7,
  effGetParamName = 8,
  effSetSampleRate = 10,
  effSetBlockSize = 11,
  effMainsChanged = 12,
  effEditGetRect = 13,
  effEditOpen = 14,
  effEditClose = 15,
  effEditIdle = 19,
  effEditTop = 20,
  effIdentify = 22,
  effGetChunk = 23,
  effSetChunk = 24,
  effProcessEvents = 25,
  effCanBeAutomated = 26,
  effString2Parameter = 27,
  effGetProgramNameIndexed = 29,
  effGetInputProperties = 33,
  effGetOutputProperties = 34,
  effGetPlugCategory = 35,
  effSetSpeakerArrangement = 42,
  effSetBypass = 44,
  effGetEffectName = 45,
  effGetVendorString = 47,
  effGetProductString = 48,
  effGetVendorVersion = 49,
  effVendorSpecific = 50,
  effCanDo = 51,
  effGetTailSize = 52,
  effIdle = 53,
  effGetVstVersion = 58,
  effEditKeyDown = 59,
  effEditKeyUp = 60,
  effBeginSetProgram = 67,
  effEndSetProgram = 68,
  effShellGetNextPlugin = 70,
  effStartProcess = 71,
  effStopProcess = 72,
  effSetProcessPrecision = 77,
};

// Plugin -> host opcodes
enum : int32_t {
  audioMasterAutomate = 0,
  audioMasterVersion = 1,
  audioMasterCurrentId = 2,
  audioMasterIdle = 3,
  audioMasterPinConnected = 4,
  audioMasterWantMidi = 6,
  audioMasterGetTime = 7,
  audioMasterProcessEvents = 8,
  audioMasterSetTime = 9,
  audioMasterTempoAt = 10,
  audioMasterGetNumAutomatableParameters = 11,
  audioMasterGetParameterQuantization = 12,
  audioMasterIOChanged = 13,
  audioMasterNeedIdle = 14,
  audioMasterSizeWindow = 15,
  audioMasterGetSampleRate = 16,
  audioMasterGetBlockSize = 17,
  audioMasterGetInputLatency = 18,
  audioMasterGetOutputLatency = 19,
  audioMasterGetPreviousPlug = 20,
  audioMasterGetNextPlug = 21,
  audioMasterWillReplaceOrAccumulate = 22,
  audioMasterGetCurrentProcessLevel = 23,
  audioMasterGetAutomationState = 24,
  audioMasterOfflineStart = 25,
  audioMasterOfflineRead = 26,
  audioMasterOfflineWrite = 27,
  audioMasterOfflineGetCurrentPass = 28,
  audioMasterOfflineGetCurrentMetaPass = 29,
  audioMasterSetOutputSampleRate = 30,
  audioMasterGetOutputSpeakerArrangement = 31,
  audioMasterGetVendorString = 32,
  audioMasterGetProductString = 33,
  audioMasterGetVendorVersion = 34,
  audioMasterVendorSpecific = 35,
  audioMasterSetIcon = 36,
  audioMasterCanDo = 37,
  audioMasterGetLanguage = 38,
  audioMasterOpenWindow = 39,
  audioMasterCloseWindow = 40,
  audioMasterGetDirectory = 41,
  audioMasterUpdateDisplay = 42,
  audioMasterBeginEdit = 43,
  audioMasterEndEdit = 44,
  audioMasterOpenFileSelector = 45,
  audioMasterCloseFileSelector = 46,
};

enum : intptr_t {
  kVstProcessLevelUnknown = 0,
  kVstProcessLevelUser = 1,
  kVstProcessLevelRealtime = 2,
  kVstProcessLevelPrefetch = 3,
  kVstProcessLevelOffline = 4,
};

}  // namespace vst2
