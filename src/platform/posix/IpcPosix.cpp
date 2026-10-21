#include "platform/Ipc.h"

#include "common/Util.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
extern "C" int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeoutMicros);
extern "C" int __ulock_wake(uint32_t operation, void* addr, uint64_t wakeValue);
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>

namespace yov {

namespace {

#if defined(__APPLE__)
constexpr uint32_t kUlCompareAndWaitShared = 3;
#endif

// Sleeps while *word == 0, at most ms. Spurious wake-ups are fine: callers loop.
void sleepOnWord(uint32_t* word, uint32_t ms) {
#if defined(__linux__)
  timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
  syscall(SYS_futex, word, FUTEX_WAIT, 0, &ts, nullptr, 0);  // shared futex: no PRIVATE flag
#elif defined(__APPLE__)
  __ulock_wait(kUlCompareAndWaitShared, word, 0, ms ? ms * 1000u : 1u);
#else
  usleep(std::min<uint32_t>(ms, 1) * 1000);
#endif
}

void wakeWord(uint32_t* word) {
#if defined(__linux__)
  syscall(SYS_futex, word, FUTEX_WAKE, 1, nullptr, nullptr, 0);
#elif defined(__APPLE__)
  __ulock_wake(kUlCompareAndWaitShared, word, 0);
#else
  (void)word;
#endif
}

}  // namespace

std::string ipcBaseName() {
  // macOS limits shared memory names to 31 characters: keep them short.
  static std::atomic<uint32_t> counter{0};
  char nm[64];
  snprintf(nm, sizeof nm, "/yov-%d-%u", (int)getpid(), ++counter);
  return nm;
}

std::string ipcName(const std::string& base, const char* suffix) { return base + suffix; }

// ---- SharedMemory ----

SharedMemory::~SharedMemory() { close(); }

bool SharedMemory::create(const std::string& name, size_t size) {
  close();
  shm_unlink(name.c_str());  // a leftover of a crashed process with a recycled pid
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) return false;
  if (ftruncate(fd, (off_t)size) != 0) {
    ::close(fd);
    shm_unlink(name.c_str());
    return false;
  }
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    shm_unlink(name.c_str());
    return false;
  }
  data_ = p;
  size_ = size;
  unlinkName_ = name;
  return true;
}

bool SharedMemory::open(const std::string& name, size_t size, bool readOnly) {
  close();
  int fd = shm_open(name.c_str(), readOnly ? O_RDONLY : O_RDWR, 0);
  if (fd < 0) return false;
  // Touching pages past the object's end would be a SIGBUS: refuse a size it does not have.
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < (off_t)size) {
    ::close(fd);
    return false;
  }
  void* p = mmap(nullptr, size, readOnly ? PROT_READ : PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return false;
  data_ = p;
  size_ = size;
  return true;
}

void SharedMemory::close() {
  if (data_) munmap(data_, size_);
  if (!unlinkName_.empty()) shm_unlink(unlinkName_.c_str());
  data_ = nullptr;
  size_ = 0;
  unlinkName_.clear();
}

void SharedMemory::unlinkName() {
  if (unlinkName_.empty()) return;
  shm_unlink(unlinkName_.c_str());
  unlinkName_.clear();
}

void SharedMemory::unlinkName(const std::string& name) { shm_unlink(name.c_str()); }

// ---- IpcEvent ----

IpcEvent::~IpcEvent() { close(); }

bool IpcEvent::create(const std::string&, uint32_t* word) {
  word_ = word;
  if (word_) std::atomic_ref<uint32_t>(*word_).store(0);
  return word_ != nullptr;
}

bool IpcEvent::open(const std::string&, uint32_t* word) {
  word_ = word;
  return word_ != nullptr;
}

void IpcEvent::close() { word_ = nullptr; }

bool IpcEvent::valid() const { return word_ != nullptr; }

void IpcEvent::signal() {
  if (!word_) return;
  std::atomic_ref<uint32_t>(*word_).store(1, std::memory_order_release);
  wakeWord(word_);
}

bool IpcEvent::wait(uint32_t ms) {
  if (!word_) return false;
  std::atomic_ref<uint32_t> w(*word_);
  uint64_t deadline = nowMs() + ms;
  for (;;) {
    if (w.exchange(0, std::memory_order_acq_rel) != 0) return true;
    uint64_t now = nowMs();
    if (now >= deadline) return false;
    sleepOnWord(word_, (uint32_t)(deadline - now));
  }
}

// ---- waits ----

WaitResult waitForReply(IpcEvent& ev, ChildProcess& proc, uint32_t ms) {
  uint64_t deadline = nowMs() + ms;
  for (;;) {
    uint64_t now = nowMs();
    uint32_t slice = (uint32_t)std::min<uint64_t>(deadline > now ? deadline - now : 0, 20);
    if (ev.wait(slice)) return WaitResult::Signaled;
    if (proc.exited()) return ev.wait(0) ? WaitResult::Signaled : WaitResult::ProcessEnded;
    if (nowMs() >= deadline) return WaitResult::Timeout;
  }
}

WaitResult waitForRequest(IpcEvent& ev, ProcessWatch& client, uint32_t ms) {
  if (ev.wait(ms)) return WaitResult::Signaled;
  if (client.gone()) return WaitResult::ProcessEnded;
  return WaitResult::Timeout;
}

}  // namespace yov
