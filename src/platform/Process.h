// Starting and watching the helper processes (scanner probes, bridges).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yov {

struct SpawnOptions {
  std::string cwd;
  // The child's stdout and stderr go to a pipe this process reads with readOutput().
  bool captureOutput = false;
  // Windows: start the child on the scanner's private desktop, where its windows are invisible.
  bool hiddenDesktop = false;
  // Children in a group die with this process and never show a crash dialog.
  enum class Group { None, Scan, Bridge } group = Group::None;
  // The child may bring its windows (a plugin's dialogs) to the front.
  bool allowForeground = false;
};

class ChildProcess {
 public:
  ChildProcess();
  ~ChildProcess();  // closes handles; does not kill a running child
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool start(const std::string& exe, const std::vector<std::string>& args, const SpawnOptions& opt,
             std::string& error);
  bool started() const;
  uint32_t pid() const;

  // Appends whatever output is available, without blocking.
  void readOutput(std::string& buf);
  // Waits up to ms for the child to end. True once it has ended.
  bool wait(uint32_t ms);
  bool exited() { return wait(0); }
  // code: the exit code it gets on Windows; POSIX systems kill it with SIGKILL.
  void kill(uint32_t code);

  // After it ended. Windows: the exit code (an exception code if it crashed). POSIX: the exit
  // status, or the signal number when signaled() is true.
  uint32_t exitCode() const;
  bool signaled() const;
  // "access violation (exit code 0xC0000005)", "segmentation fault (signal 11)", ...
  // codeLabel is put before the hexadecimal Windows code ("exit code ").
  std::string exitDescription(const char* codeLabel = "exit code ") const;

  void* nativeHandle() const;  // Windows: the process HANDLE

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// Watches another process by id (the bridge watches the plugin that started it).
class ProcessWatch {
 public:
  ProcessWatch();
  ~ProcessWatch();
  bool open(uint32_t pid);
  bool gone();
  void* nativeHandle() const;

 private:
  void* h_ = nullptr;
  uint32_t pid_ = 0;
  bool parent_ = false;
};

// True if there is a desktop (an X display on Linux) to open plugin editors on.
bool haveDesktop();
// The scanner's invisible desktop, used by children started with SpawnOptions::hiddenDesktop.
// Windows: a private desktop (always available). Linux: an Xvfb server, if one is installed,
// running from the first acquire to the last release. Without one, children use the normal
// display.
void hiddenDesktopAcquire();
void hiddenDesktopRelease();

// A lock shared by every process of this user (one scan at a time across DAWs).
class SystemLock {
 public:
  ~SystemLock();
  bool tryAcquire(const char* name);
  void release();

 private:
  void* h_ = nullptr;
  int fd_ = -1;
};

}  // namespace yov
