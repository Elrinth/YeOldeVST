// Crash guard around calls into untrusted plugin code.
//
// A VST2 plugin that dereferences a bad pointer raises an access violation (Windows) or a
// SIGSEGV (Linux, macOS). Without a guard that takes the whole host (the DAW) down. sehGuard()
// catches it, reports what happened, and the caller marks the plugin as dead so it is never
// called again. Windows uses structured exception handling; POSIX systems a signal handler that
// jumps back out of the faulting call.
//
// This cannot help with faults on the plugin's own threads or with heap corruption that only
// shows up later; for those the out-of-process bridge is the real answer.
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace yov {

// What SehFault::code holds for a C++ exception that escaped the plugin (the MSVC exception code;
// POSIX systems report it with the same number). Other codes are Windows exception codes or, on
// POSIX systems, signal numbers.
constexpr uint32_t kCxxExceptionCode = 0xE06D7363;

struct SehFault {
  uint32_t code = 0;
  void* address = nullptr;
  void* caller = nullptr;  // for a jump to address 0: where the bad call came from
  int accessKind = -1;      // access violations: 0 read, 1 write, 8 execute (DEP / NX)
  std::string describe() const;
};

using SehFn = void (*)(void* ctx);
bool sehInvoke(SehFn fn, void* ctx, SehFault* fault);

// Runs f(); returns false (and fills *fault) if it faulted.
template <class F>
bool sehGuard(F&& f, SehFault* fault) {
  using Fn = std::remove_reference_t<F>;
  return sehInvoke([](void* p) { (*static_cast<Fn*>(p))(); }, (void*)&f, fault);
}

// Name of an exception code / signal ("access violation", "segmentation fault"...).
const char* exceptionName(uint32_t code);

// For the helper processes: called when something faults outside any guard (a plugin thread,
// a destructor), right before the process dies of it. The handler reports; the process then
// ends with the fault's code (Windows) or by the signal (POSIX), so the parent sees a crash.
using FaultHandler = void (*)(const SehFault& fault);
void setUnhandledFaultHandler(FaultHandler handler);

}  // namespace yov
