#include "usbip_attach.h"

#include "log.h"

#include <filesystem>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <spawn.h>
  #include <sys/wait.h>
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

    auto argv = attach_command(options, busid, options.use_low_latency_mode);
    int code = run_process(argv);
#ifdef _WIN32
    if (code != 0 && options.use_low_latency_mode) {
      log::info("attach: retrying without newer usbip-win2 options");
      argv = attach_command(options, busid, false);
      code = run_process(argv);
    }
#endif
    if (code != 0) {
      log::error("attach: '", argv[0], "' failed (exit ", code, "). Is usbip installed and are we running as administrator/root?");
      return false;
    }
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

  int run_process(const std::vector<std::string> &argv) {
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

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
      log::error("attach: cannot start ", argv[0], " (error ", GetLastError(), ")");
      return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
  }
#else
  int run_process(const std::vector<std::string> &argv) {
    if (argv.empty()) {
      return -1;
    }
    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const auto &arg : argv) {
      raw.push_back(const_cast<char *>(arg.c_str()));
    }
    raw.push_back(nullptr);

    pid_t pid = 0;
    if (posix_spawnp(&pid, raw[0], nullptr, nullptr, raw.data(), environ) != 0) {
      log::error("attach: cannot start ", argv[0]);
      return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }
#endif

}  // namespace inputline
