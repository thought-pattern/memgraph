// Copyright 2026 Memgraph Ltd.
//
// Licensed as a Memgraph Enterprise file under the Memgraph Enterprise
// License (the "License"); by using this file, you agree to be bound by the terms of the License, and you may not use
// this file except in compliance with the License. You may obtain a copy of the License at https://memgraph.com/legal.
//
//

#include "auth/module.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#ifdef MG_HAS_LIBSECCOMP
#include <seccomp.h>
#endif
#include <spdlog/spdlog.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "utils/nofile_limit.hpp"

extern char **environ;

namespace {

/////////////////////////////////////////////////////////////////////////
// Constants used for starting and communicating with the target process.
/////////////////////////////////////////////////////////////////////////

const int kPipeReadEnd = 0;
const int kPipeWriteEnd = 1;

const int kCommunicationToModuleFd = 1000;
const int kCommunicationFromModuleFd = 1001;

const int kTerminateTimeoutSec = 5;

////////////////////////////////////
// Security functions and constants.
////////////////////////////////////

#ifdef MG_HAS_LIBSECCOMP
const std::vector<int> kSeccompSyscallsBlacklist = {
    SCMP_SYS(mknod),         SCMP_SYS(mount),        SCMP_SYS(setuid),
    SCMP_SYS(stime),         SCMP_SYS(ptrace),       SCMP_SYS(setgid),
    SCMP_SYS(acct),          SCMP_SYS(umount),       SCMP_SYS(setpgid),
    SCMP_SYS(chroot),        SCMP_SYS(setreuid),     SCMP_SYS(setregid),
    SCMP_SYS(sethostname),   SCMP_SYS(settimeofday), SCMP_SYS(setgroups),
    SCMP_SYS(swapon),        SCMP_SYS(reboot),       SCMP_SYS(setpriority),
    SCMP_SYS(ioperm),        SCMP_SYS(syslog),       SCMP_SYS(iopl),
    SCMP_SYS(vhangup),       SCMP_SYS(vm86old),      SCMP_SYS(swapoff),
    SCMP_SYS(setdomainname), SCMP_SYS(adjtimex),     SCMP_SYS(init_module),
    SCMP_SYS(delete_module), SCMP_SYS(setfsuid),     SCMP_SYS(setfsgid),
    SCMP_SYS(setresuid),     SCMP_SYS(vm86),         SCMP_SYS(setresgid),
    SCMP_SYS(capset),        SCMP_SYS(setreuid),     SCMP_SYS(setregid),
    SCMP_SYS(setgroups),     SCMP_SYS(setresuid),    SCMP_SYS(setresgid),
    SCMP_SYS(setuid),        SCMP_SYS(setgid),       SCMP_SYS(setfsuid),
    SCMP_SYS(setfsgid),      SCMP_SYS(pivot_root),   SCMP_SYS(sched_setaffinity),
    SCMP_SYS(clock_settime), SCMP_SYS(kexec_load),   SCMP_SYS(mknodat),
    SCMP_SYS(unshare),
#ifdef SYS_seccomp
    SCMP_SYS(seccomp),
#endif
};

bool SetupSeccomp() {
  // Initialize the seccomp context.
  scmp_filter_ctx ctx;
  ctx = seccomp_init(SCMP_ACT_ALLOW);
  if (ctx == nullptr) return false;

  // Add all general blacklist rules.
  for (auto syscall_num : kSeccompSyscallsBlacklist) {
    if (seccomp_rule_add(ctx, SCMP_ACT_KILL, syscall_num, 0) != 0) {
      seccomp_release(ctx);
      return false;
    }
  }

  // Load the context for the current process.
  auto ret = seccomp_load(ctx);

  // Free the context and return success/failure.
  seccomp_release(ctx);
  return ret == 0;
}
#else
bool SetupSeccomp() { return true; }
#endif

bool SetLimit(int resource, rlim_t n) {
  struct rlimit limit;
  limit.rlim_cur = limit.rlim_max = n;
  return setrlimit(resource, &limit) == 0;
}

#ifdef __linux__
// close_range(2); the toolchain's glibc 2.31 headers predate it, the number is the same on x86_64 and aarch64.
constexpr long kSysCloseRange = 436;
#endif

bool CloseFdRangeImpl(int first, int last, bool allow_procfs) {
  if (first > last) {
    return true;
  }
#ifdef __linux__
  if (allow_procfs && syscall(kSysCloseRange, first, last, 0) == 0) return true;
#endif
  if (allow_procfs) {
    DIR *dir = opendir("/proc/self/fd");
    if (dir == nullptr) dir = opendir("/dev/fd");
    if (dir != nullptr) {
      const int dir_fd = dirfd(dir);
      std::vector<int> to_close;
      errno = 0;
      while (const auto *entry = readdir(dir)) {
        char *endptr = nullptr;
        const long fd = std::strtol(entry->d_name, &endptr, 10);
        if (endptr != entry->d_name && *endptr == '\0' && fd >= first && fd <= last && fd != dir_fd) {
          to_close.push_back(static_cast<int>(fd));
        }
        errno = 0;
      }
      const int read_err = errno;
      closedir(dir);
      if (read_err == 0) {
        for (int fd : to_close) {
          (void)close(fd);
        }
        return true;
      }
    }
  }

  constexpr long kMaxFallbackScanFd = 4'194'304L;
  long effective_last = -1;
  if (static_cast<long>(last) - static_cast<long>(first) <= kMaxFallbackScanFd) {
    effective_last = static_cast<long>(last);
  } else {
    struct rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0 &&
        rl.rlim_cur <= static_cast<rlim_t>(kMaxFallbackScanFd)) {
      effective_last = std::min<long>(last, static_cast<long>(rl.rlim_cur) - 1);
    } else {
      const long sc = sysconf(_SC_OPEN_MAX);
      if (sc > 0 && sc <= kMaxFallbackScanFd) {
        effective_last = std::min<long>(last, sc - 1);
      }
    }
  }
  if (effective_last < first) {
    return false;
  }
  for (long fd = first; fd <= effective_last; ++fd) {
    (void)close(static_cast<int>(fd));
  }
  for (long fd = first; fd <= effective_last; ++fd) {
    if (fcntl(static_cast<int>(fd), F_GETFD) != -1 || errno != EBADF) {
      return false;
    }
  }
  return true;
}

////////////////////////////////////////////////////
// Target function used to start the module process.
////////////////////////////////////////////////////

int Target(void *arg) {
  // NOTE: (D)LOG shouldn't be used here because it wasn't initialized in this
  // process and something really bad could happen.

  // Get a pointer to the passed arguments.
  auto *ta = reinterpret_cast<memgraph::auth::TargetArguments *>(arg);
  if (ta == nullptr || !ta->argv.IsNullTerminated() || !ta->envp.IsNullTerminated()) {
    std::cerr << "Invalid execution arguments for auth module!" << std::endl;
    return EXIT_FAILURE;
  }

  // Redirect `stdin` to `/dev/null`.
  int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    std::cerr << "Couldn't open \"/dev/null\" for auth module stdin because of: " << strerror(errno) << " (" << errno
              << ")!" << std::endl;
    return EXIT_FAILURE;
  }
  if (dup2(fd, STDIN_FILENO) != STDIN_FILENO) {
    close(fd);
    std::cerr << "Couldn't attach \"/dev/null\" to auth module stdin because of: " << strerror(errno) << " (" << errno
              << ")!" << std::endl;
    return EXIT_FAILURE;
  }
  if (fd != STDIN_FILENO) {
    close(fd);
  }

  // Change the current directory to the module directory.
  if (chdir(ta->module_executable_path.parent_path().c_str()) != 0) {
    std::cerr << "Couldn't change directory to " << ta->module_executable_path.parent_path()
              << " for auth module stdin because of: " << strerror(errno) << " (" << errno << ")!" << std::endl;
    return EXIT_FAILURE;
  }

  // Connect the communication input pipe.
  if (dup2(ta->pipe_to_module, kCommunicationToModuleFd) != kCommunicationToModuleFd) {
    std::cerr << "Couldn't attach communication to module pipe to auth module "
                 "because of: "
              << strerror(errno) << " (" << errno << ")!" << std::endl;
    return EXIT_FAILURE;
  }

  // Connect the communication output pipe.
  if (dup2(ta->pipe_from_module, kCommunicationFromModuleFd) != kCommunicationFromModuleFd) {
    std::cerr << "Couldn't attach communication from module pipe to auth "
                 "module because of: "
              << strerror(errno) << " (" << errno << ")!" << std::endl;
    return EXIT_FAILURE;
  }

  // Don't leak the server's non-CLOEXEC fds (e.g. listening sockets, log file) into the module.
  static_assert(kCommunicationFromModuleFd == kCommunicationToModuleFd + 1);
  if (!CloseFdRangeImpl(STDERR_FILENO + 1, kCommunicationToModuleFd - 1, true) ||
      !CloseFdRangeImpl(kCommunicationFromModuleFd + 1, std::numeric_limits<int>::max(), true)) {
    std::cerr << "Couldn't close inherited file descriptors for auth module!" << std::endl;
    return EXIT_FAILURE;
  }

  // Disable core dumps.
  if (!SetLimit(RLIMIT_CORE, 0)) {
    std::cerr << "Couldn't disable core dumps for auth module!" << std::endl;
    return EXIT_FAILURE;
  }

  // The server raises its soft open files limit; the module gets the original one (at least covering the comm fds).
  if (const auto &original = memgraph::utils::OriginalNoFileLimit()) {
    struct rlimit restored = *original;
    const rlim_t min_comm_nofile = static_cast<rlim_t>(kCommunicationFromModuleFd + 1);
    if (restored.rlim_cur != RLIM_INFINITY && restored.rlim_cur < min_comm_nofile) {
      restored.rlim_cur =
          (restored.rlim_max == RLIM_INFINITY) ? min_comm_nofile : std::min<rlim_t>(restored.rlim_max, min_comm_nofile);
    }
    if (setrlimit(RLIMIT_NOFILE, &restored) != 0) {
      std::cerr << "Couldn't restore the open files limit for auth module!\n";
      return EXIT_FAILURE;
    }
  }

  // Ignore SIGINT.
  struct sigaction action;
  // `sa_sigaction` must be cleared before `sa_handler` is set because on some
  // platforms the two are a union.
  action.sa_sigaction = nullptr;
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (sigaction(SIGINT, &action, nullptr) != 0) {
    std::cerr << "Couldn't ignore SIGINT for auth module because of: " << strerror(errno) << " (" << errno << ")!"
              << std::endl;
    return EXIT_FAILURE;
  }

  // Setup seccomp.
  if (!SetupSeccomp()) {
    std::cerr << "Couldn't enable seccomp for auth module!" << std::endl;
    return EXIT_FAILURE;
  }

  execve(ta->module_executable_path.c_str(), ta->argv.Data(), ta->envp.Data());

  // If the `execve` call succeeded then the process will exit from that call
  // and won't reach this piece of code ever.
  std::cerr << "Couldn't start auth module because of: " << strerror(errno) << " (" << errno << ")!" << std::endl;

  return EXIT_FAILURE;
}

/////////////////////////////////////////////////////
// Function used to send data to the started process.
/////////////////////////////////////////////////////

/// The data that is being sent to the module process is always a newline
/// terminated JSON encoded string.

bool EnsureNonBlockingFd(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
#ifdef F_SETNOSIGPIPE
  (void)fcntl(fd, F_SETNOSIGPIPE, 1);
#endif
  if ((flags & O_NONBLOCK) != 0) return true;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int RemainingDeadlineMs(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return 0;
  const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
  return rem > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(rem);
}

bool CreateCloexecPipe(std::array<int, 2> &pipe_fds) {
#ifdef __linux__
  return pipe2(pipe_fds.data(), O_CLOEXEC) == 0;
#else
  if (pipe(pipe_fds.data()) != 0) {
    return false;
  }
  for (int fd : pipe_fds) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
      close(pipe_fds[0]);
      close(pipe_fds[1]);
      pipe_fds = {-1, -1};
      return false;
    }
  }
  return true;
#endif
}

}  // namespace

namespace memgraph::auth {

bool CloseInheritedFdRange(int first, int last, bool allow_procfs) {
  return CloseFdRangeImpl(first, last, allow_procfs);
}

bool ExecStringArray::Build(const std::vector<std::string> &entries) {
  try {
    storage = entries;
    pointers.clear();
    pointers.reserve(storage.size() + 1);
    for (auto &item : storage) {
      pointers.push_back(item.data());
    }
    pointers.push_back(nullptr);
    return true;
  } catch (const std::bad_alloc &) {
    storage.clear();
    pointers.clear();
    return false;
  }
}

bool ExecStringArray::BuildFromNullTerminated(const char *const *entries) {
  try {
    storage.clear();
    pointers.clear();
    if (entries != nullptr) {
      size_t count = 0;
      while (entries[count] != nullptr) {
        ++count;
      }
      storage.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        storage.emplace_back(entries[i]);
      }
    }
    pointers.reserve(storage.size() + 1);
    for (auto &item : storage) {
      pointers.push_back(item.data());
    }
    pointers.push_back(nullptr);
    return true;
  } catch (const std::bad_alloc &) {
    storage.clear();
    pointers.clear();
    return false;
  }
}

bool PutAuthModuleRequestWithDeadline(int fd, const nlohmann::json &data,
                                      std::chrono::steady_clock::time_point deadline) {
  if (fd < 0 || !EnsureNonBlockingFd(fd)) {
    return false;
  }

  std::string encoded;
  try {
    encoded = data.dump();
  } catch (const nlohmann::json::type_error &) {
    return false;
  }

  if (encoded.empty()) return false;
  if (*encoded.rbegin() != '\n') {
    encoded.push_back('\n');
  }

  size_t put = 0;
  while (put < encoded.size()) {
    const ssize_t written = write(fd, encoded.data() + put, encoded.size() - put);
    if (written > 0) {
      put += static_cast<size_t>(written);
      continue;
    }
    if (written == -1 && errno == EINTR) {
      continue;
    }
    if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const int wait_ms = RemainingDeadlineMs(deadline);
      if (wait_ms <= 0) {
        return false;
      }
      struct pollfd desc{};
      desc.fd = fd;
      desc.events = POLLOUT;
      const int prc = poll(&desc, 1, wait_ms);
      if (prc < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (prc == 0 || (desc.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

nlohmann::json GetAuthModuleResponseWithDeadline(int fd, std::string &rx_buffer,
                                                 std::chrono::steady_clock::time_point deadline,
                                                 size_t max_response_bytes) {
  if (fd < 0 || max_response_bytes == 0 || !EnsureNonBlockingFd(fd)) {
    return {};
  }

  while (true) {
    if (const auto nl = rx_buffer.find('\n'); nl != std::string::npos) {
      if (nl > max_response_bytes) {
        rx_buffer.clear();
        return {};
      }
      const std::string line = rx_buffer.substr(0, nl);
      rx_buffer.erase(0, nl + 1);
      try {
        return nlohmann::json::parse(line);
      } catch (const nlohmann::json::parse_error &) {
        return {};
      }
    }

    if (rx_buffer.size() >= max_response_bytes) {
      rx_buffer.clear();
      return {};
    }

    char chunk[4096];
    const size_t allowed = std::min(sizeof(chunk), max_response_bytes - rx_buffer.size() + 1);
    const ssize_t got = read(fd, chunk, allowed);
    if (got > 0) {
      rx_buffer.append(chunk, static_cast<size_t>(got));
      if (rx_buffer.find('\n') == std::string::npos && rx_buffer.size() > max_response_bytes) {
        rx_buffer.clear();
        return {};
      }
      continue;
    }
    if (got == 0) {
      return {};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const int wait_ms = RemainingDeadlineMs(deadline);
      if (wait_ms <= 0) {
        return {};
      }
      struct pollfd desc{};
      desc.fd = fd;
      desc.events = POLLIN;
      const int prc = poll(&desc, 1, wait_ms);
      if (prc < 0) {
        if (errno == EINTR) continue;
        return {};
      }
      if (prc == 0) {
        return {};
      }
      continue;
    }
    return {};
  }
}

Module::Module(const std::filesystem::path &module_executable_path) {
  if (!module_executable_path.empty()) {
    module_executable_path_ = std::filesystem::absolute(module_executable_path);
  }
}

void Module::CloseAllPipes() noexcept {
  for (int &fd : pipe_to_module_) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }
  for (int &fd : pipe_from_module_) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }
}

bool Module::Startup() {
  // Check whether the process is alive.
  if (pid_ != -1 && waitpid(pid_, &status_, WNOHANG | WUNTRACED) == 0) {
    return true;
  }

  // Cleanup leftover state.
  Shutdown();

  // Setup communication pipes.
  if (!CreateCloexecPipe(pipe_to_module_)) {
    spdlog::error(
        "Couldn't create communication pipe from the database to "
        "the auth module!");
    return false;
  }
  if (!CreateCloexecPipe(pipe_from_module_)) {
    spdlog::error(
        "Couldn't create communication pipe from the auth module to "
        "the database!");
    CloseAllPipes();
    return false;
  }

  // Set the target arguments and prepare complete null-terminated argv/envp arrays before spawning.
  target_arguments_->module_executable_path = module_executable_path_;
  if (!target_arguments_->argv.Build({module_executable_path_.string()}) ||
      !target_arguments_->envp.BuildFromNullTerminated(environ)) {
    spdlog::error("Couldn't allocate execution arguments or environment for the auth module!");
    CloseAllPipes();
    return false;
  }
  target_arguments_->pipe_to_module = pipe_to_module_[kPipeReadEnd];
  target_arguments_->pipe_from_module = pipe_from_module_[kPipeWriteEnd];

  // Create the process.
#ifdef __linux__
  uint8_t *stack_top = stack_.get() + kStackSizeBytes;
  pid_ = clone(Target, stack_top, CLONE_VFORK, target_arguments_.get());
#else
  pid_ = fork();
  if (pid_ == 0) {
    _exit(Target(target_arguments_.get()));
  }
#endif
  if (pid_ == -1) {
    spdlog::error("Couldn't start the auth module process!");
    CloseAllPipes();
    return false;
  }

  // Close child-only pipe ends in the parent immediately after spawning so EOF is observed if the child exits early.
  close(pipe_to_module_[kPipeReadEnd]);
  close(pipe_from_module_[kPipeWriteEnd]);
  pipe_to_module_[kPipeReadEnd] = -1;
  pipe_from_module_[kPipeWriteEnd] = -1;

  // Check whether the process is still running.
  if (waitpid(pid_, &status_, WNOHANG | WUNTRACED) != 0) {
    spdlog::error("The auth module process couldn't be started!");
    if (WIFSTOPPED(status_)) {
      Shutdown();
    } else {
      CloseAllPipes();
      pid_ = -1;
      status_ = 0;
      rx_buffer_.clear();
    }
    return false;
  }

  if (!EnsureNonBlockingFd(pipe_to_module_[kPipeWriteEnd]) || !EnsureNonBlockingFd(pipe_from_module_[kPipeReadEnd])) {
    spdlog::error("Couldn't configure non-blocking pipes for the auth module process!");
    Shutdown();
    return false;
  }
  rx_buffer_.clear();

  return true;
}

nlohmann::json Module::Call(nlohmann::json params, int timeout_millisec) {
  auto guard = std::lock_guard{lock_};

  if (!params.is_object()) return {};

  // Ensure that the module is up and running.
  if (!Startup()) return {};

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_millisec));

  static constexpr auto kMemgraphCallIdKey = "memgraph_call_id";
  const auto call_id = ++call_id_;
  params[kMemgraphCallIdKey] = call_id;

  // Put the request to the module process under a single non-blocking request deadline.
  if (!PutAuthModuleRequestWithDeadline(pipe_to_module_[kPipeWriteEnd], params, deadline)) {
    spdlog::error("Couldn't send data to the auth module process!");
    Shutdown();
    return {};
  }

  // Read responses until we get one with matching memgraph_call_id (skip stale) under the same request deadline.
  const int kMaxResponses = 1000;
  for (int i = 0; i < kMaxResponses; ++i) {
    auto ret = GetAuthModuleResponseWithDeadline(
        pipe_from_module_[kPipeReadEnd], rx_buffer_, deadline, kMaxAuthModuleResponseBytes);
    if (ret.is_null()) {
      spdlog::error("Couldn't receive data from the auth module process!");
      Shutdown();
      return {};
    }
    if (!ret.is_object()) {
      spdlog::error("Data received from the auth module is of wrong type!");
      continue;
    }
    if (ret.contains(kMemgraphCallIdKey) && ret[kMemgraphCallIdKey].is_number_integer() &&
        ret[kMemgraphCallIdKey].get<uint64_t>() == call_id) {
      return ret;
    }
    // Stale or wrong call_id, skip and read next response
    spdlog::trace("Auth module response had wrong memgraph_call_id, reading next.");
  }
  spdlog::error("Auth module did not return response with matching memgraph_call_id after {} attempts.", kMaxResponses);
  Shutdown();
  return {};
}

void Module::Shutdown() {
  if (pid_ == -1) return;

  // Try to terminate the process gracefully in `kTerminateTimeoutSec`.
  for (int i = 0; i < kTerminateTimeoutSec * 100; ++i) {
    if (i % 10 == 0) {
      spdlog::info("Terminating the auth module process with pid {}", pid_);
      kill(pid_, SIGTERM);
    }
    int ret = waitpid(pid_, &status_, WNOHANG | WUNTRACED);
    if (ret == pid_ || ret == -1) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // If the process is still alive, kill it and wait for it to die.
  if (waitpid(pid_, &status_, WNOHANG | WUNTRACED) == 0) {
    spdlog::warn("Killing the auth module process with pid {}", pid_);
    kill(pid_, SIGKILL);
    waitpid(pid_, &status_, 0);
  }

  // Close any remaining open pipe ends and reset state.
  CloseAllPipes();
  pid_ = -1;
  status_ = 0;
  rx_buffer_.clear();
}

Module::~Module() { Shutdown(); }

}  // namespace memgraph::auth
