// What the helper executable (scanner probe, bridge) needs from the OS and its window system.
// Windows: Win32. Linux: X11. Window handles are passed around as void*: an HWND, or an X11
// window id cast to a pointer (which is also how VST2 editors receive their parent).
#pragma once

#include <cstdint>
#include <string>

namespace yov::helper {

// Process setup: no system error dialogs, COM (Windows); Xlib thread support (X11).
// probe: a scanner probe (also no crash dialogs; it reports crashes itself).
void init(bool probe);
// Writes to stdout unbuffered (the scanner reads it even if we crash right after).
void writeStdout(const std::string& s);
// Ends the process at once: no static destructors, no teardown of whatever the plugin left.
[[noreturn]] void exitNow(int code);
// Audio-thread scheduling (MMCSS "Pro Audio" / real-time priority where allowed).
void boostAudioThread();
// Handles pending window-system messages of this thread (at most `budget` of them).
void pumpMessages(int budget);
// True if there is a display to open editors on.
bool haveDisplay();

// Bridge: the window a plugin's editor opens in, a child of a window of the client process.
void* containerCreate(int64_t parent, int width, int height);
void containerResize(void* container, int width, int height);
// Size of the first window the plugin created inside the container.
bool containerChildSize(void* container, int& width, int& height);
void containerDestroy(void* container);

// Probe: a shown but out-of-sight window for the editor test.
void* probeWindowCreate();
void probeWindowDestroy(void* window);
// Probe: watches for windows this process opens other than the probe window (nag screens,
// licence prompts, error boxes), reports each one's text once and dismisses it.
void startPopupEater(void (*report)(const std::string& text));

}  // namespace yov::helper
