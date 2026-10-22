# Porting notes

Everything that differs between operating systems lives in `src/platform/` behind small
interfaces; the rest of the code (scanner, bridge protocol, CLAP plugin, both GUIs' logic) is
shared. This page lists the interfaces, what each platform has, and what is left for macOS.

## The interfaces

| Interface | What it does | Windows | Linux | macOS |
| --- | --- | --- | --- | --- |
| `common/Util.h` (OS parts) | paths, files, directory listing, environment, clocks, log | `win/UtilWin.cpp` | `posix/UtilPosix.cpp` | same file, `__APPLE__` paths (`~/Library/...`, `st_mtimespec`, `pthread_threadid_np`) |
| `common/Seh.h` | crash guard around plugin calls, unhandled-fault hook for the helpers | SEH (`win/SehWin.cpp`) | signal handler + `siglongjmp` (`posix/SehPosix.cpp`) | same file, Darwin register names |
| `common/ModuleInfo.h` | architecture and entry points of a plugin binary, without loading it | PE | ELF | Mach-O, universal binaries, `.vst` bundles (written, untested) |
| `platform/DynLib.h` | load a plugin | `LoadLibraryEx` | `dlopen` | `dlopen` of the bundle's executable |
| `platform/Process.h` | helper processes, scan lock, hidden scan display | job objects, private desktop | `fork`+`execve`, `PR_SET_PDEATHSIG`, Xvfb | `fork`+`execve`; no death signal (the bridge polls its parent), no hidden display |
| `platform/Ipc.h` | shared memory and wake-ups for the bridge | named mappings and events | `shm_open` + futex words | `shm_open` (names <= 31 chars) + `__ulock_wait` / `__ulock_wake` |
| `platform/Embedded.h` | the helper exe inside the plugin binary | RCDATA resources | `.incbin` | `.incbin` should work; a bundle could also carry it in `Contents/MacOS` |
| `platform/Helper.h` | what the probe and the bridge need from the window system | Win32 | X11 (`x11/HelperX11.cpp`) | **missing** (`posix/HelperNoUi.cpp` stubs) |
| `plugin/imgui/GuiWindow.h` | the plugin window's native side for the Dear ImGui GUI | Win32 + WGL (opt-in) | X11 + GLX | **missing** |

## What macOS still needs

1. **`GuiWindow` for Cocoa** (`plugin/imgui/CocoaGuiWindow.mm`): an `NSView` added to the host's
   view (`clap_window_t::cocoa`), an OpenGL view (`NSOpenGLView`, deprecated but working) or a
   Metal layer with ImGui's Metal backend, input forwarded to ImGui, an `NSTimer` for the tick, a
   child `NSView` for the plugin's editor. `CLAP_WINDOW_API_COCOA` is already what `Gui::windowApi()`
   returns there.
2. **`Helper` for Cocoa**: the probe's editor test window (an off-screen `NSWindow`), the popup
   eater (watch `[NSApp windows]` of the probe process), the bridge's editor container.
3. **Bridged editors**: a view cannot be a child of another process's view on macOS. The usual
   answers are a separate floating window owned by the helper process, or sharing the editor's
   layer through `CARemoteLayerServer` / `CAContext` (private API). This is the one real design
   decision left; in-process plugins are not affected.
4. **Packaging**: the CLAP is a bundle (`YeOldeVST.clap/Contents/MacOS/YeOldeVST`), clap-wrapper
   builds the VST3 bundle; both need code signing and notarization, and the helper inside them
   too. A universal (x86_64 + arm64) build needs one helper per architecture; arm64 hosts can only
   load arm64 plugins in-process, so x86_64 plugins would always be bridged (through Rosetta).
5. **Plugin folders**: `/Library/Audio/Plug-Ins/VST` and `~/Library/Audio/Plug-Ins/VST` are already
   the defaults; the scanner treats `.vst` folders as plugins.

## Linux notes

- The GUI and the bridge share the X server: the bridge creates its editor container as a child of
  the plugin window's X11 window id, so bridged editors embed like on Windows.
- The GUI runs from the host's CLAP timer and fd callbacks (`clap.timer-support`,
  `clap.posix-fd-support`); it never blocks on vblank and puts the host's GL context back after
  drawing.
- 32-bit Linux plugins would need a 32-bit helper (`-m32` build of `yov_host`, multilib); the
  scanner already reports them as "Needs helper".
- `YEOLDEVST_GUI_DEBUG=1` logs the X events the window gets; `YEOLDEVST_SCALE` overrides the
  window scale (default: `Xft.dpi` / 96).
