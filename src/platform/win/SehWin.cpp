// Windows side of common/Seh.h: structured exception handling.
#include "common/Seh.h"

#include <windows.h>
#include <malloc.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>

namespace yov {

const char* exceptionName(uint32_t code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "float denormal operand";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
    case EXCEPTION_FLT_INEXACT_RESULT: return "float inexact result";
    case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
    case EXCEPTION_FLT_OVERFLOW: return "float overflow";
    case EXCEPTION_FLT_STACK_CHECK: return "float stack check";
    case EXCEPTION_FLT_UNDERFLOW: return "float underflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case 0xE06D7363: return "unhandled C++ exception";
    case 0xC0000409: return "stack buffer overrun";
    case 0xC0000374: return "heap corruption";
    case 0xC0000135: return "missing DLL";
    case 0xC0000142: return "DLL initialization failed";
    default: return "exception";
  }
}

static std::string where(void* address) {
  char mod[MAX_PATH] = "?";
  uintptr_t offset = (uintptr_t)address;
  HMODULE h = nullptr;
  if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)address, &h)) {
    wchar_t full[MAX_PATH];
    if (GetModuleFileNameW(h, full, MAX_PATH)) {
      const wchar_t* base = wcsrchr(full, L'\\');
      // UTF-8, so Cyrillic / other non-ANSI module names survive into reports.
      WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : full, -1, mod, sizeof mod, nullptr, nullptr);
    }
    offset = (uintptr_t)address - (uintptr_t)h;
  }
  char buf[MAX_PATH + 32];
  snprintf(buf, sizeof buf, "%s+0x%llX", mod, (unsigned long long)offset);
  return buf;
}

std::string SehFault::describe() const {
  char buf[768];
  if (address == nullptr && caller) {
    snprintf(buf, sizeof buf, "%s (0x%08X): call through a null function pointer from %s", exceptionName(code), code,
             where(caller).c_str());
  } else if (accessKind == 8) {
    snprintf(buf, sizeof buf, "%s (0x%08X): executed data memory at %s (blocked by DEP)", exceptionName(code), code,
             where(address).c_str());
  } else {
    snprintf(buf, sizeof buf, "%s (0x%08X) in %s", exceptionName(code), code, where(address).c_str());
  }
  return buf;
}

static void sehCapture(EXCEPTION_POINTERS* ep, SehFault* fault) {
  fault->code = ep->ExceptionRecord->ExceptionCode;
  fault->address = ep->ExceptionRecord->ExceptionAddress;
  fault->caller = nullptr;
  fault->accessKind = -1;
  if (fault->code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 1)
    fault->accessKind = (int)ep->ExceptionRecord->ExceptionInformation[0];
  if (fault->address == nullptr) {
    // A call to address 0 faults before the callee runs: the return address is on top of the stack.
#if defined(_M_IX86)
    void** sp = (void**)ep->ContextRecord->Esp;
#else
    void** sp = (void**)ep->ContextRecord->Rsp;
#endif
    if (sp && !IsBadReadPtr(sp, sizeof(void*))) fault->caller = (uint8_t*)*sp - 1;
  }
}

static int filter(EXCEPTION_POINTERS* ep, SehFault* fault) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  // Informational exceptions that are meant to be continued (debug strings, thread naming).
  if (code == 0x40010006 /*DBG_PRINTEXCEPTION_C*/ || code == 0x4001000A /*DBG_PRINTEXCEPTION_WIDE_C*/ ||
      code == 0x406D1388 /*MS_VC_EXCEPTION thread name*/)
    return EXCEPTION_CONTINUE_EXECUTION;
  if (fault) sehCapture(ep, fault);
  return EXCEPTION_EXECUTE_HANDLER;
}

bool sehInvoke(SehFn fn, void* ctx, SehFault* fault) {
  __try {
    fn(ctx);
    return true;
  } __except (filter(GetExceptionInformation(), fault)) {
    if (fault && fault->code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
    return false;
  }
}

static FaultHandler gUnhandled = nullptr;

static LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
  SehFault f;
  sehCapture(ep, &f);
  if (gUnhandled) gUnhandled(f);
  TerminateProcess(GetCurrentProcess(), f.code);
  return EXCEPTION_EXECUTE_HANDLER;
}

void setUnhandledFaultHandler(FaultHandler handler) {
  gUnhandled = handler;
  SetUnhandledExceptionFilter(unhandledFilter);
}

void sehShutdown() {}

}  // namespace yov
