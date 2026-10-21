# YeOldeVST

A CLAP and VST3 plugin for Windows that loads old **VST 1.x / 2.x** plugins (`.dll`), both
**64-bit and 32-bit**, and shows them inside its own window. It lets you keep using old VST2
instruments and effects in DAWs that only load VST3 or CLAP.

It comes in three flavours, all in the same file:

- **YeOldeVST Instrument**: MIDI in, stereo out
- **YeOldeVST Instrument (multi-out)**: MIDI in, 16 stereo outputs (for Kontakt "16out", drum
  machines and other multi-output instruments)
- **YeOldeVST Effect**: stereo in, MIDI in, stereo out

Every flavour also has a MIDI output, which carries whatever the hosted plugin sends
(arpeggiators, MIDI generators).

## Features

- **Plugin browser.** Add your VST2 folders, scan them, then search and filter the results
  (working, problems, instruments, effects, 32-bit). Copies of the same plugin (for example 25
  skins of one synth, or its 32- and 64-bit versions) are grouped into one row that expands to
  show each file; loading a group picks its best working copy, preferring 64-bit. Double-click a
  plugin to load it.
- **Editor column.** Shows whether each plugin's editor was tested and how it went. Editors that
  passed are never tested again. Untick *Test editors* for a much faster scan; tick it later and
  the next scan tests only the editors that haven't been tested. *Rescan failed* re-tests only the
  plugins that crashed, hung or wouldn't load.
- **Unicode paths.** Folders and files with Cyrillic or other non-Latin names work everywhere.
  Old plugins that only understand the system code page are given the folder's short (8.3) name.
- **Fail-safe scanning.** Each plugin is tested in its own throwaway helper process with a timeout,
  so a plugin that crashes or hangs never takes the DAW down. The test does the following:
  - loads the plugin
  - plays a note through it
  - checks the output for NaN or very loud samples
  - measures CPU use
  - lists its parameters and presets
  - saves and restores its state
  - opens and closes its editor
  - unloads it

  The result says exactly where a plugin failed, for example *"crashed while opening its editor:
  access violation in Foo.dll+0x1234"*.
- **Popups are absorbed during scans.** Scans run on a hidden desktop, and a watchdog dismisses
  nag screens, licence prompts and error boxes, so they never interrupt you. What they said goes
  into the report, for example *"showed a popup: Playtime VST is not supported in this host"*. When
  you load a plugin for real, its dialogs appear normally.
- **32-bit plugins** run in a 32-bit helper process (a bridge). Their editor is embedded in the
  window just like a 64-bit plugin's. The 32-bit helper runs without DEP, as hosts of that era did,
  because many old plugins (ATL/WTL editors, copy protection) execute code from data memory.
- **Sandboxed mode** runs a 64-bit plugin out of process too. If it crashes, only the helper dies.
  The track goes silent, and you can press *Reload plugin* to restore it with its last state.
- **Crash guard for in-process plugins.** Every call into the plugin is wrapped in an SEH guard. A
  plugin that faults is switched off instead of crashing the DAW. This is best effort: faults on
  the plugin's own threads can't be caught. Use sandboxed mode for plugins you don't trust.
- **Plugin editor or generic sliders.** You get the plugin's own editor, or a generic slider view
  for plugins without one (or whose editor is broken). The toolbar has a preset picker.
- **DAW integration:**
  - parameter automation: up to 1024 parameters, named after the plugin's own
  - full state recall, using the chunk, or parameters plus the current program
  - latency reporting
  - MIDI in and out, including SysEx
  - transport and tempo
- **Moved plugins are found again.** If a project points at a plugin that has moved, YeOldeVST
  finds it by ID and name. If the plugin is missing, its settings are kept untouched until it is
  back.
- **Ships as a single file.** The helper executables are embedded in the plugin binary and
  unpacked to `%LOCALAPPDATA%\YeOldeVST\bin` on first use.

Settings, the scan database (`plugins.json`) and a log are kept in `%APPDATA%\YeOldeVST`. All
instances share them.

## Installing

Download `YeOldeVST-1.0.0-win64.zip` from the
[Releases page](https://github.com/Elrinth/YeOldeVST/releases) and unpack it.

Copy `YeOldeVST.clap` to `%LOCALAPPDATA%\Programs\Common\CLAP` or
`C:\Program Files\Common Files\CLAP`. Copy the `YeOldeVST.vst3` folder to
`%LOCALAPPDATA%\Programs\Common\VST3` or `C:\Program Files\Common Files\VST3`.

## Building

You need Visual Studio 2022 (with the x86 and x64 C++ tools) and CMake 3.21 or newer.

```
git clone --recursive https://github.com/Elrinth/YeOldeVST.git
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The build has these outputs in `build/Release`:

- `YeOldeVST.clap`
- the `YeOldeVST.vst3` bundle
- `YeOldeVST-Host64.exe`
- `yov_clap_test.exe`
- `YovTest64.dll`, a test VST2 plugin (6 outputs, echoes notes back as MIDI)

`YeOldeVST-Host32.exe` and `YovTest32.dll` are built by a nested Win32 sub-build into `build/host32/Release`.

Dependencies are git submodules, pinned:

- [CLAP](https://github.com/free-audio/clap) 1.2.9
- [clap-wrapper](https://github.com/free-audio/clap-wrapper) 0.9.1, which provides the VST3 build
- [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) 3.8.0 (MIT licensed)

The VST2 interface (`src/vst2/aeffect.h`) is a clean-room description of the binary ABI. It does
not use the withdrawn Steinberg VST2 SDK.

## Command-line helper

`YeOldeVST-Host64.exe` (and `-Host32.exe`) can also be used directly.

Scan folders into a separate database:

```
YeOldeVST-Host64.exe --scan "D:\Audio\Vsts" --db scan.json [--rescan] [--retry] [--no-editor] [--parallel 6] [--timeout 40]
```

Test a single plugin:

```
YeOldeVST-Host64.exe --probe plugin.dll --editor
```

Play a note through a plugin and write the result to a WAV file:

```
YeOldeVST-Host32.exe --render plugin.dll out.wav
```

## Testing

`yov_clap_test.exe` is a minimal CLAP host.

Load a VST2 through YeOldeVST, play it, then save and restore its state into a second instance:

```
yov_clap_test.exe build/Release/YeOldeVST.clap render instrument "D:\Audio\Vsts\TAL-NoiseMaker.dll"
```

Open the GUI and save a screenshot:

```
yov_clap_test.exe build/Release/YeOldeVST.clap gui instrument shot.bmp "D:\Audio\Vsts\TAL-NoiseMaker.dll"
```

To run host test tools against a real VST2, set `YEOLDEVST_TEST_AUTOLOAD=<path to dll>`. New
instances then start with that plugin already loaded. With it set, Steinberg's VST3 validator
passes all 94 tests, both for an in-process 64-bit plugin and a bridged 32-bit plugin.

## How it works

```
DAW ──CLAP/VST3──> YeOldeVST plugin (64-bit)
                     ├─ in-process:  Vst2Plugin (SEH-guarded calls)
                     └─ bridged:     shared memory + events ──> YeOldeVST-Host32/64.exe --bridge
                                                               (plugin, its editor, audio thread)
Scanner ──spawns──> YeOldeVST-Host32/64.exe --probe <dll>   (one throwaway process per plugin)
```

These are the main source files:

- `src/vst2/aeffect.h`: the VST2 ABI
- `src/host/Vst2Plugin.*`: hosts one VST2 plugin, with guarded calls, the host callback, and
  stack-aligned thunks for 32-bit plugins
- `src/scan/Scanner.*`: runs the parallel, sandboxed scan
- `src/hostexe/Probe.cpp`: the step-by-step plugin test, including the popup eater
- `src/hostexe/Bridge.cpp`: the bridge server
- `src/plugin/BridgeClient.cpp`: the bridge client
- `src/common/BridgeProtocol.h`: the shared-memory layout used by the bridge
- `src/plugin/Plugin.*`: the CLAP plugin (audio, MIDI, parameters, state)
- `src/plugin/Gui.*`: the Win32 user interface

## Known limitations

- Windows only. The plugin is 64-bit only; 32-bit VST2 plugins are hosted through the bridge.
- 64-bit processes always run with DEP, so a 64-bit plugin that executes data memory can't work
  in-process or sandboxed (none in testing did).
- VST3 hosts show parameter names only after a plugin is loaded. They update when you load another
  plugin, because the VST3 wrapper rebuilds the parameter list.

## License

YeOldeVST is released under the [MIT License](LICENSE). It includes CLAP, clap-wrapper and the
VST 3 SDK, all MIT licensed; see [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt).

VST is a registered trademark of Steinberg Media Technologies GmbH.
