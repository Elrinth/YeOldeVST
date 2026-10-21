// POSIX side of common/Seh.h.
//
// A process-wide handler for the crash signals jumps back (siglongjmp) into the innermost
// sehGuard() of the faulting thread. Signals that arrive outside a guard go to whatever handler
// was installed before ours (the DAW's own, or the default action), so hosts that use signals
// themselves (Java-based DAWs, crash reporters) keep working. Guarded threads get an alternate
// signal stack so a stack overflow can be caught too.
#include "common/Seh.h"

#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace yov {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP};
struct sigaction gPrevious[32];
FaultHandler gUnhandled = nullptr;

struct GuardFrame {
  sigjmp_buf jb;
  SehFault* fault;
  GuardFrame* prev;
};
// initial-exec: read in the signal handler, which must not trigger a lazy TLS allocation (we are
// a dlopen'ed library; the general model may malloc on a thread's first access).
#if defined(__GNUC__)
#define YOV_TLS_IE __attribute__((tls_model("initial-exec")))
#else
#define YOV_TLS_IE
#endif
thread_local GuardFrame* tlsFrame YOV_TLS_IE = nullptr;
thread_local void* tlsAltStack YOV_TLS_IE = nullptr;

void capture(int sig, siginfo_t* si, void* ucv, SehFault* f) {
  f->code = (uint32_t)sig;
  f->address = nullptr;
  f->caller = nullptr;
  f->accessKind = -1;
  auto* uc = static_cast<ucontext_t*>(ucv);
  void* pc = nullptr;
  void* sp = nullptr;
  long err = -1;
#if defined(__linux__) && defined(__x86_64__)
  if (uc) {
    pc = (void*)uc->uc_mcontext.gregs[REG_RIP];
    sp = (void*)uc->uc_mcontext.gregs[REG_RSP];
    err = (long)uc->uc_mcontext.gregs[REG_ERR];
  }
#elif defined(__linux__) && defined(__i386__)
  if (uc) {
    pc = (void*)uc->uc_mcontext.gregs[REG_EIP];
    sp = (void*)uc->uc_mcontext.gregs[REG_ESP];
    err = (long)uc->uc_mcontext.gregs[REG_ERR];
  }
#elif defined(__linux__) && defined(__aarch64__)
  if (uc) pc = (void*)uc->uc_mcontext.pc;
#elif defined(__APPLE__) && defined(__x86_64__)
  if (uc && uc->uc_mcontext) {
    pc = (void*)uc->uc_mcontext->__ss.__rip;
    sp = (void*)uc->uc_mcontext->__ss.__rsp;
    err = (long)uc->uc_mcontext->__es.__err;
  }
#elif defined(__APPLE__) && defined(__aarch64__)
  if (uc && uc->uc_mcontext) pc = (void*)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#endif
  f->address = pc;
  if ((sig == SIGSEGV || sig == SIGBUS) && si) {
    if (err >= 0) f->accessKind = (err & 16) ? 8 : (err & 2) ? 1 : 0;  // x86 page-fault error code
    if (pc == nullptr) {
      // A call through a null function pointer: the return address is on top of the stack.
#if defined(__x86_64__) || defined(__i386__)
      if (sp) f->caller = (uint8_t*)*(void**)sp - 1;
#elif defined(__linux__) && defined(__aarch64__)
      if (uc) f->caller = (void*)(uc->uc_mcontext.regs[30] - 4);
#elif defined(__APPLE__) && defined(__aarch64__)
      if (uc && uc->uc_mcontext) f->caller = (void*)(__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss) - 4);
#endif
    } else if (f->accessKind < 0 && si->si_addr == pc) {
      f->accessKind = 8;
    }
  }
}

void chain(int sig, siginfo_t* si, void* uc) {
  struct sigaction& p = gPrevious[sig];
  if (p.sa_flags & SA_SIGINFO) {
    if (p.sa_sigaction) {
      p.sa_sigaction(sig, si, uc);
      return;
    }
  } else if (p.sa_handler != SIG_DFL && p.sa_handler != SIG_IGN && p.sa_handler) {
    p.sa_handler(sig);
    return;
  }
  // Default action: die of the signal (returning re-executes the fault with it in place).
  signal(sig, SIG_DFL);
  if (sig == SIGABRT || sig == SIGTRAP) raise(sig);
}

void onSignal(int sig, siginfo_t* si, void* uc) {
  if (GuardFrame* fr = tlsFrame) {
    tlsFrame = fr->prev;  // first: a fault inside capture() must not land in this frame again
    capture(sig, si, uc, fr->fault);
    siglongjmp(fr->jb, 1);
  }
  if (gUnhandled) {
    SehFault f;
    capture(sig, si, uc, &f);
    FaultHandler h = gUnhandled;
    gUnhandled = nullptr;  // a fault inside the handler must not recurse
    h(f);
    signal(sig, SIG_DFL);
    if (sig == SIGABRT || sig == SIGTRAP) raise(sig);
    return;
  }
  chain(sig, si, uc);
}

std::mutex gInstallM;
std::atomic<bool> gInstalled{false};

void installHandlers() {
  if (gInstalled.load(std::memory_order_acquire)) return;  // every guarded call comes here
  std::lock_guard<std::mutex> lk(gInstallM);
  if (gInstalled.load()) return;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = onSignal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  for (int s : kSignals) sigaction(s, &sa, &gPrevious[s]);
  gInstalled.store(true, std::memory_order_release);  // only now may other threads skip the lock
}

void ensureAltStack() {
  if (tlsAltStack) return;
  stack_t cur{};
  if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= 32768) {
    tlsAltStack = cur.ss_sp;  // the thread already has one (the host set it up)
    return;
  }
  const size_t size = 256 * 1024;
  void* mem = std::malloc(size);
  if (!mem) return;
  stack_t ss{};
  ss.ss_sp = mem;
  ss.ss_size = size;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, nullptr) == 0) tlsAltStack = mem;
  else std::free(mem);
}

std::string where(void* address) {
  Dl_info info{};
  if (address && dladdr(address, &info) && info.dli_fname) {
    const char* base = std::strrchr(info.dli_fname, '/');
    char buf[512];
    snprintf(buf, sizeof buf, "%s+0x%llX", base ? base + 1 : info.dli_fname,
             (unsigned long long)((uintptr_t)address - (uintptr_t)info.dli_fbase));
    return buf;
  }
  char buf[64];
  snprintf(buf, sizeof buf, "?+0x%llX", (unsigned long long)(uintptr_t)address);
  return buf;
}

}  // namespace

const char* exceptionName(uint32_t code) {
  switch (code) {
    case SIGSEGV: return "segmentation fault";
    case SIGBUS: return "bus error";
    case SIGFPE: return "arithmetic error";
    case SIGILL: return "illegal instruction";
    case SIGABRT: return "abort";
    case SIGTRAP: return "breakpoint";
    case SIGKILL: return "killed";
    case SIGTERM: return "terminated";
    case kCxxExceptionCode: return "unhandled C++ exception";
    default: return "signal";
  }
}

std::string SehFault::describe() const {
  char buf[768];
  if (code == kCxxExceptionCode) {
    snprintf(buf, sizeof buf, "%s", exceptionName(code));
  } else if (address == nullptr && caller) {
    snprintf(buf, sizeof buf, "%s (signal %u): call through a null function pointer from %s", exceptionName(code),
             code, where(caller).c_str());
  } else if (accessKind == 8) {
    snprintf(buf, sizeof buf, "%s (signal %u): executed data memory at %s", exceptionName(code), code,
             where(address).c_str());
  } else {
    snprintf(buf, sizeof buf, "%s (signal %u) in %s", exceptionName(code), code, where(address).c_str());
  }
  return buf;
}

bool sehInvoke(SehFn fn, void* ctx, SehFault* fault) {
  installHandlers();
  ensureAltStack();
  SehFault local;
  GuardFrame fr;
  fr.fault = fault ? fault : &local;
  fr.prev = tlsFrame;
  if (sigsetjmp(fr.jb, 1) != 0) return false;  // the handler already restored tlsFrame
  tlsFrame = &fr;
  try {
    fn(ctx);
  } catch (...) {
    tlsFrame = fr.prev;
    *fr.fault = SehFault{};
    fr.fault->code = kCxxExceptionCode;
    return false;
  }
  tlsFrame = fr.prev;
  return true;
}

void setUnhandledFaultHandler(FaultHandler handler) {
  installHandlers();
  ensureAltStack();
  gUnhandled = handler;
}

void sehShutdown() {
  std::lock_guard<std::mutex> lk(gInstallM);
  if (!gInstalled) return;
  gInstalled = false;
  for (int s : kSignals) {
    // Only where ours is still the active handler: the host may have installed its own since.
    struct sigaction cur;
    if (sigaction(s, nullptr, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == onSignal)
      sigaction(s, &gPrevious[s], nullptr);
  }
}

}  // namespace yov
