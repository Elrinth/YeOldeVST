// The non-window parts of platform/Helper.h on Linux / macOS.
#include "platform/Helper.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cerrno>

namespace yov::helper {

void writeStdout(const std::string& s) {
  size_t done = 0;
  while (done < s.size()) {
    ssize_t n = write(1, s.data() + done, s.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    done += (size_t)n;
  }
}

void exitNow(int code) { _exit(code); }

void boostAudioThread() {
  // Real-time scheduling needs rtprio permission (audio group); without it this fails quietly.
  sched_param sp{};
  int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
  sp.sched_priority = lo + (hi - lo) / 2;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

}  // namespace yov::helper
