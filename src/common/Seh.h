// Structured-exception guard around calls into untrusted plugin code.
//
// A VST2 plugin that dereferences a bad pointer raises an access violation. Without a guard
// that takes the whole host (the DAW) down. sehGuard() catches it, reports what happened, and
// the caller marks the plugin as dead so it is never called again.
//
// This cannot help with faults on the plugin's own threads or with heap corruption that only
// shows up later; for those the out-of-process bridge is the real answer.
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace yov {

struct SehFault {
  uint32_t code = 0;
  void* address = nullptr;
  void* caller = nullptr;  // for a jump to address 0: where the bad call came from
  int accessKind = -1;      // access violations: 0 read, 1 write, 8 execute (DEP)
  std::string describe() const;
};

using SehFn = void (*)(void* ctx);
bool sehInvoke(SehFn fn, void* ctx, SehFault* fault);

// Runs f(); returns false (and fills *fault) if it raised a structured exception.
template <class F>
bool sehGuard(F&& f, SehFault* fault) {
  using Fn = std::remove_reference_t<F>;
  return sehInvoke([](void* p) { (*static_cast<Fn*>(p))(); }, (void*)&f, fault);
}

const char* exceptionName(uint32_t code);
// Fills fault from an exception record/context (shared by the guard and unhandled filters).
void sehCapture(void* exceptionPointers, SehFault* fault);

}  // namespace yov
