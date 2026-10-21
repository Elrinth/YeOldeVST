#include "platform/Ipc.h"

#include "common/Util.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace yov {

std::string ipcBaseName() {
  static std::atomic<uint32_t> counter{0};
  char nm[160];
  snprintf(nm, sizeof nm, "Local\\YeOldeVST-%lu-%u-%llu", GetCurrentProcessId(), ++counter,
           (unsigned long long)GetTickCount64());
  return nm;
}

std::string ipcName(const std::string& base, const char* suffix) { return base + suffix; }

// ---- SharedMemory ----

SharedMemory::~SharedMemory() { close(); }

bool SharedMemory::create(const std::string& name, size_t size) {
  close();
  HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32),
                                (DWORD)size, name.c_str());
  if (!m) return false;
  void* p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!p) {
    CloseHandle(m);
    return false;
  }
  handle_ = m;
  data_ = p;
  size_ = size;
  return true;
}

bool SharedMemory::open(const std::string& name, size_t size, bool readOnly) {
  close();
  HANDLE m = OpenFileMappingA(readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!m) return false;
  void* p = MapViewOfFile(m, readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!p) {
    CloseHandle(m);
    return false;
  }
  handle_ = m;
  data_ = p;
  size_ = size;
  return true;
}

void SharedMemory::close() {
  if (data_) UnmapViewOfFile(data_);
  if (handle_) CloseHandle(handle_);
  data_ = nullptr;
  handle_ = nullptr;
  size_ = 0;
}

void SharedMemory::unlinkName() {}
void SharedMemory::unlinkName(const std::string&) {}

// ---- IpcEvent ----

IpcEvent::~IpcEvent() { close(); }

bool IpcEvent::create(const std::string& name, uint32_t*) {
  close();
  handle_ = CreateEventA(nullptr, FALSE, FALSE, name.c_str());
  return handle_ != nullptr;
}

bool IpcEvent::open(const std::string& name, uint32_t*) {
  close();
  handle_ = OpenEventA(EVENT_ALL_ACCESS, FALSE, name.c_str());
  return handle_ != nullptr;
}

void IpcEvent::close() {
  if (handle_) CloseHandle(handle_);
  handle_ = nullptr;
}

bool IpcEvent::valid() const { return handle_ != nullptr; }

void IpcEvent::signal() { SetEvent(handle_); }

bool IpcEvent::wait(uint32_t ms) { return WaitForSingleObject(handle_, ms) == WAIT_OBJECT_0; }

// ---- waits ----

WaitResult waitForReply(IpcEvent& ev, ChildProcess& proc, uint32_t ms) {
  HANDLE waits[2] = {ev.nativeHandle(), proc.nativeHandle()};
  ULONGLONG deadline = GetTickCount64() + ms;
  for (;;) {
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) return WaitResult::Timeout;
    DWORD r = MsgWaitForMultipleObjects(2, waits, FALSE, (DWORD)(deadline - now), QS_SENDMESSAGE);
    if (r == WAIT_OBJECT_0) return WaitResult::Signaled;
    if (r == WAIT_OBJECT_0 + 1) return WaitResult::ProcessEnded;
    if (r == WAIT_OBJECT_0 + 2) {
      MSG m;
      PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      continue;
    }
    return WaitResult::Timeout;
  }
}

WaitResult waitForRequest(IpcEvent& ev, ProcessWatch& client, uint32_t ms) {
  HANDLE waits[2] = {ev.nativeHandle(), client.nativeHandle()};
  DWORD r = MsgWaitForMultipleObjects(2, waits, FALSE, ms, QS_ALLINPUT);
  if (r == WAIT_OBJECT_0) return WaitResult::Signaled;
  if (r == WAIT_OBJECT_0 + 1) return WaitResult::ProcessEnded;
  if (r == WAIT_OBJECT_0 + 2) return WaitResult::Messages;
  return WaitResult::Timeout;
}

}  // namespace yov
