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

std::string SehFault::describe() const {
  char buf[512];
  char mod[MAX_PATH] = "?";
  uintptr_t offset = 0;
  HMODULE h = nullptr;
  if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)address, &h)) {
    char full[MAX_PATH];
    if (GetModuleFileNameA(h, full, MAX_PATH)) {
      const char* base = strrchr(full, '\\');
      snprintf(mod, sizeof mod, "%s", base ? base + 1 : full);
    }
    offset = (uintptr_t)address - (uintptr_t)h;
  }
  snprintf(buf, sizeof buf, "%s (0x%08X) in %s+0x%llX", exceptionName(code), code, mod, (unsigned long long)offset);
  return buf;
}

static int filter(EXCEPTION_POINTERS* ep, SehFault* fault) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  // Informational exceptions that are meant to be continued (debug strings, thread naming).
  if (code == 0x40010006 /*DBG_PRINTEXCEPTION_C*/ || code == 0x4001000A /*DBG_PRINTEXCEPTION_WIDE_C*/ ||
      code == 0x406D1388 /*MS_VC_EXCEPTION thread name*/)
    return EXCEPTION_CONTINUE_EXECUTION;
  if (fault) {
    fault->code = code;
    fault->address = ep->ExceptionRecord->ExceptionAddress;
  }
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

}  // namespace yov
