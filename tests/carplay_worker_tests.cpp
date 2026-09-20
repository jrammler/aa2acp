#include "aa2acp/bridge/carplay_worker.hpp"

#include <cassert>
#include <csignal>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  // Reap the killed grandchild ourselves instead of depending on the test
  // environment's init process to do so promptly.
  assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
  int report[2]{};
  assert(pipe2(report, O_CLOEXEC) == 0);
  const auto leader = fork();
  assert(leader >= 0);
  if (leader == 0) {
    close(report[0]);
    if (setpgid(0, 0) != 0)
      _exit(127);
    const auto descendant = fork();
    if (descendant < 0)
      _exit(127);
    if (descendant == 0) {
      close(report[1]);
      for (;;)
        pause();
    }
    if (write(report[1], &descendant, sizeof(descendant)) !=
        static_cast<ssize_t>(sizeof(descendant)))
      _exit(127);
    for (;;)
      pause();
  }

  close(report[1]);
  pid_t descendant{};
  assert(read(report[0], &descendant, sizeof(descendant)) ==
         static_cast<ssize_t>(sizeof(descendant)));
  close(report[0]);
  aa2acp::bridge::kill_worker_process_group(leader);
  int status{};
  assert(waitpid(leader, &status, 0) == leader);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  assert(waitpid(descendant, &status, 0) == descendant);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}
