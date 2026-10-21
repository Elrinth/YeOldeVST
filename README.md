# YeOldeVST

A CLAP and VST3 plugin that loads old **VST 1.x / 2.x** plugins and shows them inside its own
window. It lets you keep using old VST2 instruments and effects in DAWs that only load VST3 or CLAP.

- **Windows:** hosts Windows VST2 plugins (`.dll`), both **64-bit and 32-bit**.
- **Linux:** hosts native Linux VST2 plugins (`.so`, 64-bit). Windows plugins on Linux (through
  Wine) are not supported.

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
  On Windows, old plugins that only understand the system code page are given the folder's short
  (8.3) name.
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
  access violation in Foo.dll+0x1234"* (on Linux: *"segmentation fault (signal 11) in
  foo.so+0x1234"*).
- **Popups are absorbed during scans.** Scans run on a hidden desktop (on Linux: a private Xvfb
  display, if Xvfb is installed), and a watchdog dismisses nag screens, licence prompts and error
  boxes, so they never interrupt you. What they said goes
  into the report, for example *"showed a popup: Playtime VST is not supported in this host"*. When
  you load a plugin for real, its dialogs appear normally.
- **32-bit plugins** run in a 32-bit helper process (a bridge). Their editor is embedded in the
  window just like a 64-bit plugin's. The 32-bit helper runs without DEP, as hosts of that era did,
  because many old plugins (ATL/WTL editors, copy protection) execute code from data memory.
- **Sandboxed mode** runs a 64-bit plugin out of process too. If it crashes, only the helper dies.
  The track goes silent, and you can press *Reload plugin* to restore it with its last state.
- **Crash guard for in-process plugins.** Every call into the plugin is wrapped in a guard (SEH on
  Windows, a signal handler on Linux). A plugin that faults is switched off instead of crashing
  the DAW. This is best effort: faults on
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
  unpacked on first use, to `%LOCALAPPDATA%\YeOldeVST\bin` on Windows and
  `~/.local/share/YeOldeVST/bin` on Linux.

Settings, the scan database (`plugins.json`) and a log are kept in `%APPDATA%\YeOldeVST`
(Windows) or `~/.config/YeOldeVST` (Linux). All instances share them.

## The window

On Windows the window is native Win32. On Linux it is drawn with
[Dear ImGui](https://github.com/ocornut/imgui) in an OpenGL child of the DAW's window, with the
same pages, texts and behaviour; the hosted plugin's editor is a real X11 child window, also for
bridged plugins. The ImGui window can be tried on Windows too: set the environment variable
`YEOLDEVST_GUI=imgui` before starting the DAW.

On Linux the window needs the DAW to offer CLAP's timer and fd support (current CLAP hosts do;
the VST3 build gets them from the host's run loop).

## Installing

Download `YeOldeVST-1.1.0-win64.zip` from the
[Releases page](https://github.com/Elrinth/YeOldeVST/releases) and unpack it.

Copy `YeOldeVST.clap` to `%LOCALAPPDATA%\Programs\Common\CLAP` or
`C:\Program Files\Common Files\CLAP`. Copy the `YeOldeVST.vst3` folder to
`%LOCALAPPDATA%\Programs\Common\VST3` or `C:\Program Files\Common Files\VST3`.

On Linux, build it (below), then copy `YeOldeVST.clap` to `~/.clap` and the `YeOldeVST.vst3`
folder to `~/.vst3`. The usual Linux VST2 folders that exist (`~/.vst`, `~/.lxvst`,
`/usr/lib/vst`, `/usr/lib/lxvst`, `$VST_PATH` ...) are suggested on first start.

## Building

### Windows

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

### Linux

You need GCC 11 or newer (or Clang), CMake 3.21 or newer, Ninja or make, and the X11 and OpenGL
development files. On Debian / Ubuntu:

```
sudo apt install build-essential cmake ninja-build libx11-dev libgl-dev
git clone --recursive https://github.com/Elrinth/YeOldeVST.git
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The outputs are `build/YeOldeVST.clap`, `build/Release/YeOldeVST.vst3`, `build/YeOldeVST-Host64`,
`build/yov_clap_test` and `build/YovTest64.so` (the test plugin, here with a small X11 editor).
With `xvfb` installed, scans run on a hidden display.

Dependencies are git submodules, pinned:

- [CLAP](https://github.com/free-audio/clap) 1.2.9
- [clap-wrapper](https://github.com/free-audio/clap-wrapper) 0.9.1, which provides the VST3 build
- [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) 3.8.0 (MIT licensed)

The VST2 interface (`src/vst2/aeffect.h`) is a clean-room description of the binary ABI. It does
not use the withdrawn Steinberg VST2 SDK.

## Command-line helper

`YeOldeVST-Host64.exe` (and `-Host32.exe`; `YeOldeVST-Host64` on Linux) can also be used directly.

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

`YOV_TEST_SANDBOXED=1` loads the plugin in the bridge process, `YOV_TEST_GENERIC=1` shows the
generic editor. On Linux the same commands work with `.so` plugins; the GUI test then runs an X11
window with the timer and fd support a CLAP host provides.

To run host test tools against a real VST2, set `YEOLDEVST_TEST_AUTOLOAD=<path to plugin>`. New
instances then start with that plugin already loaded. With it set, Steinberg's VST3 validator
passes all 141 tests, on Windows (in-process 64-bit and bridged 32-bit plugins) and on Linux.

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
- `src/hostexe/Probe.cpp`: the step-by-step plugin test
- `src/hostexe/Bridge.cpp`: the bridge server
- `src/plugin/BridgeClient.cpp`: the bridge client
- `src/common/BridgeProtocol.h`: the shared-memory layout used by the bridge
- `src/common/ModuleInfo.*`: reads a plugin binary's architecture and entry points (PE, ELF,
  Mach-O) without loading it
- `src/plugin/Plugin.*`: the CLAP plugin (audio, MIDI, parameters, state)
- `src/plugin/BrowserModel.*`: what the plugin list shows (rows, groups, texts, colours)
- `src/plugin/win/Win32Gui.*`: the Win32 user interface
- `src/plugin/imgui/`: the Dear ImGui user interface and its native windows (X11 + GLX,
  Win32 + WGL)
- `src/platform/`: everything that differs between operating systems, behind small interfaces
  with a Windows (`win/`) and a POSIX (`posix/`, `x11/`) implementation: files and paths, the
  crash guard, loading libraries, helper processes, shared memory and events, what the probe and
  the bridge need from the window system (popup eater, editor container), the embedded helpers

## Known limitations

- The plugin is 64-bit only. On Windows, 32-bit VST2 plugins are hosted through the bridge; on
  Linux there is no 32-bit helper yet, so 32-bit `.so` plugins show "Needs helper".
- Linux hosts Linux VST2 plugins only; Windows plugins through Wine are not supported.
- macOS: the platform layer has POSIX and Mach-O code paths, but there is no Cocoa window yet, so
  it does not build a usable plugin.
- 64-bit processes always run with DEP, so a 64-bit plugin that executes data memory can't work
  in-process or sandboxed (none in testing did).
- VST3 hosts show parameter names only after a plugin is loaded. They update when you load another
  plugin, because the VST3 wrapper rebuilds the parameter list.

## License

YeOldeVST is released under the [MIT License](LICENSE). It includes CLAP, clap-wrapper, the
VST 3 SDK and Dear ImGui, all MIT licensed; see [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt).

VST is a registered trademark of Steinberg Media Technologies GmbH.
