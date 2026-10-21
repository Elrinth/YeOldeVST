// Shared memory and cross-process wake-ups for the bridge.
//
// Windows uses named file mappings and named auto-reset events. POSIX systems use shm_open()
// mappings, and the "events" are 32-bit words inside the shared block that the waiting side
// sleeps on (futex on Linux, __ulock on macOS). Callers pass both a name and the word; each
// implementation uses what it needs.
#pragma once

#include "platform/Process.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace yov {

// A fresh base name for one bridge's objects ("Local\YeOldeVST-..." / "/yov-...").
std::string ipcBaseName();
// Name of a further object derived from a base name (suffix like "-creq" or "-big7").
std::string ipcName(const std::string& base, const char* suffix);

class SharedMemory {
 public:
  SharedMemory() = default;
  ~SharedMemory();
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  // The creator owns the name: on POSIX systems it is removed when the creator closes it.
  bool create(const std::string& name, size_t size);
  // Fails if the object is smaller than size.
  bool open(const std::string& name, size_t size, bool readOnly = false);
  void close();
  // POSIX: removes the name now; mappings stay valid and the memory goes away with the last one,
  // so nothing is left in /dev/shm if a process dies. Windows: nothing to do (named objects
  // disappear with their last handle anyway).
  void unlinkName();
  static void unlinkName(const std::string& name);
  void* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  void* data_ = nullptr;
  size_t size_ = 0;
  void* handle_ = nullptr;
  std::string unlinkName_;
};

class IpcEvent {
 public:
  IpcEvent() = default;
  ~IpcEvent();
  IpcEvent(const IpcEvent&) = delete;
  IpcEvent& operator=(const IpcEvent&) = delete;

  bool create(const std::string& name, uint32_t* word);
  bool open(const std::string& name, uint32_t* word);
  void close();
  bool valid() const;

  void signal();
  // Waits up to ms; true if it was signalled (the signal is consumed).
  bool wait(uint32_t ms);
  // Drops a signal that arrived earlier.
  void reset() { wait(0); }
  void* nativeHandle() const { return handle_; }

 private:
  void* handle_ = nullptr;
  uint32_t* word_ = nullptr;
};

enum class WaitResult { Signaled, ProcessEnded, Messages, Timeout };

// Client side: waits for a reply or for the bridge process to end. While waiting, Windows
// handles messages other processes send to this thread synchronously (the bridge's editor
// window is a child of ours). Returns Signaled, ProcessEnded or Timeout.
WaitResult waitForReply(IpcEvent& ev, ChildProcess& proc, uint32_t ms);
// Bridge side: waits for a request, the client process ending, or (Windows) window messages
// for this thread. Returns any of the results.
WaitResult waitForRequest(IpcEvent& ev, ProcessWatch& client, uint32_t ms);

}  // namespace yov
