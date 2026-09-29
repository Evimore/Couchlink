#include "usbip_attach.h"

#include "log.h"

#include <filesystem>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
extern char **environ;
#endif

namespace inputline {

  std::string default_usbip_executable() {
#ifdef _WIN32
    for (const char *candidate : {R"(C:\Program Files\USBip\usbip.exe)", R"(C:\Program Files\usbip-win2\usbip.exe)"}) {
      std::error_code error;
      if (std::filesystem::exists(candidate, error)) {
        return candidate;
      }
    }
    return "usbip.exe";
#else
    return "usbip";
#endif
  }

  std::vector<std::string> attach_command(const AttachOptions &options, const std::string &busid, bool with_extras) {
    std::vector<std::string> argv {
      options.executable.empty() ? default_usbip_executable() : options.executable,
      "--tcp-port",
      std::to_string(options.port),
      "attach",
      "-r",
      options.host,
      "-b",
      busid,
    };
#ifdef _WIN32
    if (with_extras) {
      // usbip-win2 0.9.8+: fail fast instead of retrying forever, and favour
      // latency over throughput for this tiny interrupt stream.
      argv.insert(argv.end(), {"--once", "--receive-mode", "low-latency"});
    }
#else
    (void) with_extras;
#endif
    return argv;
  }

  bool run_usbip_attach(const AttachOptions &options, const std::string &busid) {
    if (!options.enabled) {
      log::info("attach: automatic attach disabled; attach ", busid, " yourself");
      return true;
    }

    auto run = [&busid](const std::vector<std::string> &argv) {
      std::string command;
      for (const auto &arg : argv) {
        command += (command.empty() ? "" : " ") + arg;
      }
      log::debug("attach: running ", command);
      std::string output;
      const int code = run_process(argv, &output);
      // usbip's own words are the best clue when attaching fails.
      while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
      }
      if (!output.empty()) {
        if (code != 0) {
          log::warn("attach: usbip said (exit ", code, "): ", output);
        } else {
          log::debug("attach: usbip said: ", output);
        }
      }
      (void) busid;
      return code;
    };

    auto argv = attach_command(options, busid, options.use_low_latency_mode);
    int code = run(argv);
#ifdef _WIN32
    if (code != 0 && options.use_low_latency_mode) {
      log::info("attach: retrying without newer usbip-win2 options");
      argv = attach_command(options, busid, false);
      code = run(argv);
    }
#endif
    if (code != 0) {
      log::error("attach: '", argv[0], "' failed (exit ", code, "). Is usbip-win2 installed, and does InputLine run as administrator/root?");
      return false;
    }
    log::info("attach: plugged in ", busid);
    return true;
  }

#ifdef _WIN32
  namespace {
    std::wstring widen(const std::string &text) {
      if (text.empty()) {
        return {};
      }
      const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(static_cast<std::size_t>(length), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
      return out;
    }

    /** Quote one argument following the MSVC CommandLineToArgvW rules. */
    std::wstring quote(const std::wstring &arg) {
      if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
      }
      std::wstring out = L"\"";
      std::size_t backslashes = 0;
      for (wchar_t c : arg) {
        if (c == L'\\') {
          ++backslashes;
        } else if (c == L'"') {
          out.append(backslashes * 2 + 1, L'\\');
          out.push_back(c);
          backslashes = 0;
        } else {
          out.append(backslashes, L'\\');
          out.push_back(c);
          backslashes = 0;
        }
      }
      out.append(backslashes * 2, L'\\');
      out.push_back(L'"');
      return out;
    }
  }  // namespace

  int run_process(const std::vector<std::string> &argv, std::string *output) {
    if (argv.empty()) {
      return -1;
    }
    std::wstring command_line;
    for (const auto &arg : argv) {
      if (!command_line.empty()) {
        command_line.push_back(L' ');
      }
      command_line += quote(widen(arg));
    }

    // Collect stdout and stderr through one pipe; stdin reads nothing.
    SECURITY_ATTRIBUTES inherit {};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    HANDLE nul = INVALID_HANDLE_VALUE;
    const bool capture = output != nullptr && CreatePipe(&read_end, &write_end, &inherit, 0);
    if (capture) {
      SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
      nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    if (capture) {
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = nul;
      startup.hStdOutput = write_end;
      startup.hStdError = write_end;
    }
    PROCESS_INFORMATION process {};
    const BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, capture ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD start_error = GetLastError();
    if (capture) {
      CloseHandle(write_end);  // our copy: reading ends when the child exits
      if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
      }
    }
    if (!started) {
      if (capture) {
        CloseHandle(read_end);
      }
      log::error("attach: cannot start ", argv[0], " (error ", start_error, ")");
      return -1;
    }
    if (capture) {
      char buffer[512];
      DWORD got = 0;
      while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        if (output->size() < 16384) {
          output->append(buffer, got);
        }
      }
      CloseHandle(read_end);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
  }
#else
  int run_process(const std::vector<std::string> &argv, std::string *output) {
    if (argv.empty()) {
      return -1;
    }
    int pipe_fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const bool capture = output != nullptr && ::pipe(pipe_fds) == 0;
    if (capture) {
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
      posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }
    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const auto &arg : argv) {
      raw.push_back(const_cast<char *>(arg.c_str()));
    }
    raw.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, raw[0], capture ? &actions : nullptr, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture) {
      ::close(pipe_fds[1]);
    }
    if (spawned != 0) {
      if (capture) {
        ::close(pipe_fds[0]);
      }
      log::error("attach: cannot start ", argv[0]);
      return -1;
    }
    if (capture) {
      char buffer[512];
      ssize_t got = 0;
      while ((got = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (output->size() < 16384) {
          output->append(buffer, static_cast<std::size_t>(got));
        }
      }
      ::close(pipe_fds[0]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }
#endif

}  // namespace inputline
