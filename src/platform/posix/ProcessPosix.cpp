#include "platform/Process.h"

#include "common/Seh.h"
#include "common/Util.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <poll.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern char** environ;

namespace yov {

namespace {

// ---- the scanner's hidden display (Xvfb) ----
std::mutex gHiddenM;
int gHiddenRefs = 0;
pid_t gXvfb = 0;
std::string gHiddenDisplay;  // ":93" while an Xvfb runs

std::string hiddenDisplay() {
  std::lock_guard<std::mutex> lk(gHiddenM);
  return gHiddenDisplay;
}

// A pipe whose ends are close-on-exec from the start, so a child some other DAW thread forks at
// the same moment cannot inherit them.
bool cloexecPipe(int fds[2]) {
#if defined(__linux__)
  return pipe2(fds, O_CLOEXEC) == 0;
#else
  if (pipe(fds) != 0) return false;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return true;
#endif
}

long maxFdToClose() {
  long maxFd = sysconf(_SC_OPEN_MAX);
  return (maxFd < 0 || maxFd > 65536) ? 65536 : maxFd;
}

// In a freshly forked child (async-signal-safe calls only): default signal handling and an
// empty mask whatever the DAW set up, and none of the DAW's descriptors (sockets, audio
// devices...) apart from 0-2 and `keep`.
void resetChild(int keep, long maxFd) {
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  for (int s : {SIGPIPE, SIGCHLD, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGINT, SIGTERM}) signal(s, SIG_DFL);
  for (int fd = 3; fd < (int)maxFd; ++fd)
    if (fd != keep) close(fd);
}

#if defined(__linux__)
// Starts Xvfb and waits until it accepts connections. display: ":N", or null to let it pick a
// free one. Returns the display number, or -1.
int spawnXvfb(const char* exe, const char* display, bool abstractOnly, pid_t& pidOut) {
  int fds[2];
  if (!cloexecPipe(fds)) return -1;
  const pid_t parent = getpid();
  const long maxFd = maxFdToClose();
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(126);
    if (fds[1] != 3) dup2(fds[1], 3);  // the copy is not close-on-exec
    else fcntl(3, F_SETFD, 0);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 1);
      dup2(devnull, 2);
    }
    resetChild(3, maxFd);
    // -displayfd: Xvfb writes the display number to fd 3 once it accepts connections.
    const char* args[16];
    int n = 0;
    args[n++] = exe;
    if (display) args[n++] = display;
    for (const char* a : {"-displayfd", "3", "-nolisten", "tcp", "-screen", "0", "1600x1200x24"}) args[n++] = a;
    if (abstractOnly) {
      args[n++] = "-nolisten";
      args[n++] = "unix";
    }
    args[n] = nullptr;
    execv(exe, (char* const*)args);
    _exit(127);
  }
  close(fds[1]);
  std::string got;
  uint64_t deadline = nowMs() + 5000;
  while (nowMs() < deadline && got.find('\n') == std::string::npos) {
    pollfd p{fds[0], POLLIN, 0};
    if (poll(&p, 1, 100) > 0) {
      char buf[32];
      ssize_t n = read(fds[0], buf, sizeof buf);
      if (n <= 0) break;
      got.append(buf, (size_t)n);
    }
  }
  close(fds[0]);
  int num = got.find('\n') == std::string::npos ? -1 : atoi(got.c_str());
  if (num < 0) {
    ::kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
    return -1;
  }
  pidOut = pid;
  return num;
}
#endif

void startXvfb() {
#if defined(__linux__)
  const char* exe = nullptr;
  for (const char* p : {"/usr/bin/Xvfb", "/usr/local/bin/Xvfb"})
    if (access(p, X_OK) == 0) exe = p;
  if (!exe) return;
  pid_t pid = 0;
  int num = spawnXvfb(exe, nullptr, false, pid);
  if (num < 0) {
    // No usable /tmp/.X11-unix (WSLg mounts it without the sticky bit): listen on the abstract
    // socket only, on a display number nobody uses.
    for (int d = 99; d < 140 && num < 0; ++d) {
      std::string sock = "/tmp/.X11-unix/X" + std::to_string(d), lock = "/tmp/.X" + std::to_string(d) + "-lock";
      if (access(sock.c_str(), F_OK) == 0 || access(lock.c_str(), F_OK) == 0) continue;
      std::string disp = ":" + std::to_string(d);
      num = spawnXvfb(exe, disp.c_str(), true, pid);
    }
  }
  if (num < 0) {
    logLine("scan: could not start Xvfb for the hidden display; probes use the normal display");
    return;
  }
  gXvfb = pid;
  gHiddenDisplay = ":" + std::to_string(num);
  logLine("scan: hidden display %s (Xvfb pid %d)", gHiddenDisplay.c_str(), (int)pid);
#endif
}

}  // namespace

bool haveDesktop() {
#if defined(__APPLE__)
  return true;
#else
  const char* d = getenv("DISPLAY");
  return d && *d;
#endif
}

void hiddenDesktopAcquire() {
  std::lock_guard<std::mutex> lk(gHiddenM);
  if (gHiddenRefs++ == 0) startXvfb();
}

void hiddenDesktopRelease() {
  std::lock_guard<std::mutex> lk(gHiddenM);
  if (gHiddenRefs == 0 || --gHiddenRefs > 0) return;
  if (gXvfb > 0) {
    ::kill(gXvfb, SIGTERM);
    int st;
    for (int i = 0; i < 100 && waitpid(gXvfb, &st, WNOHANG) == 0; ++i) usleep(10000);
    if (waitpid(gXvfb, &st, WNOHANG) == 0) {
      ::kill(gXvfb, SIGKILL);
      waitpid(gXvfb, &st, 0);
    }
  }
  gXvfb = 0;
  gHiddenDisplay.clear();
}

struct ChildProcess::Impl {
  std::mutex m;  // wait() is called from the audio and main threads
  pid_t pid = 0;
  int out = -1;
  bool ended = false;
  bool signaled = false;
  bool reaped = false;
  uint32_t code = 0;
};

ChildProcess::ChildProcess() : d_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
  if (d_->out >= 0) close(d_->out);
  // Reap it if it is already gone, so it does not linger as a zombie.
  if (d_->pid > 0 && !d_->reaped) {
    int st = 0;
    waitpid(d_->pid, &st, WNOHANG);
  }
}

bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args, const SpawnOptions& opt,
                         std::string& error) {
  // Everything the child needs is prepared before fork(): between fork and exec only
  // async-signal-safe calls are allowed (the DAW is multi-threaded).
  std::vector<std::string> store;
  store.push_back(exe);
  for (auto& a : args) store.push_back(a);
  std::vector<char*> argv;
  for (auto& s : store) argv.push_back(s.data());
  argv.push_back(nullptr);
  std::string cwd = opt.cwd;
  // The environment, with DISPLAY pointing at the hidden display if asked for one.
  std::string hidden = opt.hiddenDesktop ? hiddenDisplay() : std::string();
  std::vector<std::string> envStore;
  for (char** e = environ; e && *e; ++e)
    if (hidden.empty() || strncmp(*e, "DISPLAY=", 8) != 0) envStore.push_back(*e);
  if (!hidden.empty()) envStore.push_back("DISPLAY=" + hidden);
  std::vector<char*> envp;
  for (auto& s : envStore) envp.push_back(s.data());
  envp.push_back(nullptr);
  const long maxFd = maxFdToClose();

  int outPipe[2] = {-1, -1};
  if (opt.captureOutput && !cloexecPipe(outPipe)) {
    error = "pipe failed";
    return false;
  }
  int errPipe[2] = {-1, -1};  // reports a failed exec back to us (closes on exec)
  if (!cloexecPipe(errPipe)) {
    error = "pipe failed";
    for (int fd : outPipe)
      if (fd >= 0) close(fd);
    return false;
  }
  const bool dieWithParent = opt.group != SpawnOptions::Group::None;
  const pid_t parent = getpid();

  pid_t pid = fork();
  if (pid < 0) {
    error = std::string("could not start helper (") + strerror(errno) + ")";
    for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]})
      if (fd >= 0) close(fd);
    return false;
  }
  if (pid == 0) {
    // ---- child ----
#if defined(__linux__)
    if (dieWithParent) {
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent) _exit(126);
    }
#endif
    (void)parent;
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) dup2(devnull, 0);
    if (outPipe[1] >= 0) {
      dup2(outPipe[1], 1);
      dup2(outPipe[1], 2);
    }
    for (int fd = 0; fd < 3; ++fd) fcntl(fd, F_SETFD, 0);  // in case a pipe end landed on 0-2
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
      // keep going in the inherited directory
    }
    resetChild(errPipe[1], maxFd);
    execve(argv[0], argv.data(), envp.data());
    int e = errno;
    ssize_t w = write(errPipe[1], &e, sizeof e);
    (void)w;
    _exit(127);
  }
  // ---- parent ----
  close(errPipe[1]);
  if (outPipe[1] >= 0) close(outPipe[1]);
  int childErr = 0;
  ssize_t n;
  do {
    n = read(errPipe[0], &childErr, sizeof childErr);
  } while (n < 0 && errno == EINTR);
  close(errPipe[0]);
  if (n == (ssize_t)sizeof childErr) {
    error = std::string("could not start helper (") + strerror(childErr) + ")";
    int st;
    waitpid(pid, &st, 0);
    if (outPipe[0] >= 0) close(outPipe[0]);
    return false;
  }
  if (outPipe[0] >= 0) {
    fcntl(outPipe[0], F_SETFL, fcntl(outPipe[0], F_GETFL) | O_NONBLOCK);
    d_->out = outPipe[0];
  }
  d_->pid = pid;
  return true;
}

bool ChildProcess::started() const { return d_->pid > 0; }
uint32_t ChildProcess::pid() const { return (uint32_t)d_->pid; }
void* ChildProcess::nativeHandle() const { return nullptr; }

void ChildProcess::readOutput(std::string& buf) {
  if (d_->out < 0) return;
  char tmp[4096];
  for (;;) {
    ssize_t n = read(d_->out, tmp, sizeof tmp);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    buf.append(tmp, (size_t)n);
    if (buf.size() > (8u << 20)) buf.erase(0, buf.size() - (1u << 20));  // runaway output
  }
}

bool ChildProcess::wait(uint32_t ms) {
  uint64_t deadline = nowMs() + ms;
  for (;;) {
    std::unique_lock<std::mutex> lk(d_->m);
    if (d_->pid <= 0 || d_->ended) return true;
    int st = 0;
    pid_t r = waitpid(d_->pid, &st, WNOHANG);
    if (r == d_->pid) {
      d_->ended = d_->reaped = true;
      if (WIFSIGNALED(st)) {
        d_->signaled = true;
        d_->code = (uint32_t)WTERMSIG(st);
      } else {
        d_->code = (uint32_t)WEXITSTATUS(st);
      }
      return true;
    }
    if (r < 0 && errno == ECHILD) {
      // The host ignores SIGCHLD, so children are reaped for us: only existence can be checked.
      if (::kill(d_->pid, 0) != 0 && errno == ESRCH) {
        d_->ended = d_->reaped = true;
        return true;
      }
    }
    lk.unlock();
    uint64_t now = nowMs();
    if (now >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(5, deadline - now)));
  }
}

void ChildProcess::kill(uint32_t) {
  // Under the lock: an unreaped child keeps its pid, so the signal cannot hit a recycled one.
  std::lock_guard<std::mutex> lk(d_->m);
  if (d_->pid > 0 && !d_->ended) ::kill(d_->pid, SIGKILL);
}

uint32_t ChildProcess::exitCode() const { return d_->code; }
bool ChildProcess::signaled() const { return d_->signaled; }

std::string ChildProcess::exitDescription(const char*) const {
  char b[160];
  if (d_->signaled)
    snprintf(b, sizeof b, "%s (signal %u)", exceptionName(d_->code), d_->code);
  else
    snprintf(b, sizeof b, "exited with code %u", d_->code);
  return b;
}

// ---- ProcessWatch ----

ProcessWatch::ProcessWatch() = default;
ProcessWatch::~ProcessWatch() = default;
bool ProcessWatch::open(uint32_t pid) {
  pid_ = pid;
  parent_ = getppid() == (pid_t)pid;
  return pid != 0 && (::kill((pid_t)pid, 0) == 0 || errno == EPERM);
}
bool ProcessWatch::gone() {
  if (!pid_) return false;
  if (::kill((pid_t)pid_, 0) != 0 && errno == ESRCH) return true;
  return parent_ && getppid() != (pid_t)pid_;  // re-parented: the process that started us died
}
void* ProcessWatch::nativeHandle() const { return nullptr; }

// ---- SystemLock ----

SystemLock::~SystemLock() { release(); }

bool SystemLock::tryAcquire(const char* name) {
  // A lock file: the kernel releases it if the holder dies, unlike a named semaphore.
  std::string path = joinPath(appDataDir(), std::string(name) + ".lock");
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return true;  // cannot lock: scan anyway
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

void SystemLock::release() {
  if (fd_ >= 0) {
    flock(fd_, LOCK_UN);
    close(fd_);
    fd_ = -1;
  }
}

}  // namespace yov
