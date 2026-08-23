#include "aa2acp/bridge/carplay_worker.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#include "aa2acp/bridge/logging.hpp"

namespace aa2acp::bridge {

CarPlayWorker::CarPlayWorker() {
  int control[2];
  int output[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control) != 0 ||
      pipe2(output, O_CLOEXEC) != 0)
    return;
  pid_ = fork();
  if (pid_ == 0) {
    setpgid(0, 0);
    close(control[0]);
    close(output[0]);
    dup2(output[1], STDOUT_FILENO);
    dup2(output[1], STDERR_FILENO);
    close(output[1]);
    iap2::set_pairing_confirmation_control_fd(control[1]);
    std::array<char, 8192> request{};
    for (;;) {
      const auto count = recv(control[1], request.data(), request.size(), 0);
      if (count <= 0)
        _exit(0);
      if (request[0] == '\2') {
        iap2::request_bluetooth_worker_stop();
        continue;
      }
      if (request[0] == '\4' &&
          count == static_cast<ssize_t>(
                       1 + sizeof(iap2::PairingConfirmationMessage) + 1)) {
        iap2::PairingConfirmationMessage confirmation{};
        std::memcpy(&confirmation, request.data() + 1, sizeof(confirmation));
        iap2::answer_pairing_confirmation(
            confirmation.id, request[1 + sizeof(confirmation)] != 0);
        continue;
      }
      if (request[0] != '\1')
        continue;
      iap2::reset_bluetooth_worker_stop();
      std::vector<char *> argv;
      for (char *argument = request.data() + 1;
           argument < request.data() + count;
           argument += std::strlen(argument) + 1)
        argv.push_back(argument);
      argv.push_back(nullptr);
      std::vector<std::string> arguments;
      for (const char *argument : argv)
        if (argument)
          arguments.emplace_back(argument);
      std::thread([control_fd = control[1], arguments = std::move(arguments)] {
        std::vector<char *> worker_argv;
        for (const auto &argument : arguments)
          worker_argv.push_back(const_cast<char *>(argument.data()));
        worker_argv.push_back(nullptr);
        const int result = iap2::run_bluetooth_worker(
            static_cast<int>(worker_argv.size() - 1), worker_argv.data());
        std::array<std::byte, 1 + sizeof(result)> response{};
        response[0] = std::byte{2};
        std::memcpy(response.data() + 1, &result, sizeof(result));
        send(control_fd, response.data(), response.size(), MSG_NOSIGNAL);
      }).detach();
    }
  }
  close(control[1]);
  close(output[1]);
  if (pid_ > 0) {
    control_fd_ = control[0];
    output_fd_ = output[0];
  } else {
    close(control[0]);
    close(output[0]);
  }
}

CarPlayWorker::~CarPlayWorker() {
  if (control_fd_ >= 0) {
    shutdown(control_fd_, SHUT_RDWR);
    close(control_fd_);
    control_fd_ = -1;
  }
  if (pid_ > 0) {
    int status{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    pid_t waited{};
    do {
      do {
        waited = waitpid(pid_, &status, WNOHANG);
      } while (waited < 0 && errno == EINTR);
      if (waited == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (waited == 0 && std::chrono::steady_clock::now() < deadline);
    if (waited == 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  if (output_fd_ >= 0)
    close(output_fd_);
}

int CarPlayWorker::run(std::vector<std::string> arguments,
                       const std::stop_token stop,
                       const std::atomic_bool &phone_disconnected,
                       std::atomic<pid_t> &active_child,
                       const RunHooks &hooks) {
  std::lock_guard lock(mutex_);
  if (pid_ <= 0 || control_fd_ < 0)
    return 1;
  std::string request;
  request.push_back('\1');
  for (const auto &argument : arguments)
    request.append(argument).push_back('\0');
  if (request.size() > 8192 ||
      send(control_fd_, request.data(), request.size(), MSG_NOSIGNAL) < 0)
    return 1;
  active_child = pid_;
  bool stopping = false;
  // Bound the post-stop drain: if the child wedges without closing its fds,
  // we SIGKILL it rather than blocking shutdown forever.
  std::optional<std::chrono::steady_clock::time_point> stop_deadline;
  for (;;) {
    if (stopping && stop_deadline &&
        std::chrono::steady_clock::now() > *stop_deadline) {
      // Escalate: SIGKILL the wedged child. Its death closes the output
      // pipe, which the poll loop below handles via the connection-lost
      // path (waitpid + cleanup included).
      log(LogLevel::warning)
          << "Bridge daemon: CarPlay worker did not exit after stop; "
             "killing child\n";
      ::kill(pid_, SIGKILL);
      stop_deadline.reset(); // SIGKILL is terminal; no further escalation
    }
    pollfd descriptors[]{{output_fd_, POLLIN, 0}, {control_fd_, POLLIN, 0}};
    if (poll(descriptors, 2, 100) > 0) {
      if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
          (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        hooks.on_connection_lost();
        // Reap so a SIGKILLed child does not linger as a zombie.
        int child_status{};
        while (waitpid(pid_, &child_status, 0) < 0 && errno == EINTR) {
        }
        active_child = -1;
        return 1;
      }
      if ((descriptors[0].revents & POLLIN) != 0) {
        std::array<char, 4096> output{};
        const auto count = read(output_fd_, output.data(), output.size());
        if (count > 0) {
          // Child output is already level-prefixed; preserve it verbatim.
          std::cout.write(output.data(), count);
          std::cout.flush();
        }
      }
      if ((descriptors[1].revents & POLLIN) != 0) {
        std::array<std::byte, 1 + sizeof(iap2::PairingConfirmationMessage) + 1>
            message{};
        const auto count = recv(control_fd_, message.data(), message.size(), 0);
        if (count > 0 && message[0] == std::byte{3} &&
            count == static_cast<ssize_t>(
                         1 + sizeof(iap2::PairingConfirmationMessage))) {
          iap2::PairingConfirmationMessage confirmation{};
          std::memcpy(&confirmation, message.data() + 1, sizeof(confirmation));
          hooks.on_pairing_confirmation(confirmation);
          continue;
        }
        if (count == static_cast<ssize_t>(1 + sizeof(int)) &&
            message[0] == std::byte{2}) {
          int result{};
          std::memcpy(&result, message.data() + 1, sizeof(result));
          hooks.on_pairing_reset();
          active_child = -1;
          return result;
        }
        active_child = -1;
        return 1;
      }
    }
    if (!stopping && (stop.stop_requested() || phone_disconnected.load())) {
      log(LogLevel::info) << "Bridge daemon: stopping active CarPlay session\n";
      const char stop_command = '\2';
      send(control_fd_, &stop_command, sizeof(stop_command), MSG_NOSIGNAL);
      stopping = true;
      stop_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
  }
}

bool CarPlayWorker::answer_pairing_confirmation(
    const iap2::PairingConfirmationMessage &confirmation,
    const bool confirmed) {
  std::array<std::byte, 1 + sizeof(confirmation) + 1> message{};
  message[0] = std::byte{4};
  std::memcpy(message.data() + 1, &confirmation, sizeof(confirmation));
  message[1 + sizeof(confirmation)] = confirmed ? std::byte{1} : std::byte{0};
  std::lock_guard lock(command_mutex_);
  return control_fd_ >= 0 &&
         send(control_fd_, message.data(), message.size(), MSG_NOSIGNAL) ==
             static_cast<ssize_t>(message.size());
}

} // namespace aa2acp::bridge
