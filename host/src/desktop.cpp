#include "desktop.h"

#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace couchlink::desktop {

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

    std::string narrow(const std::wstring &text) {
      if (text.empty()) {
        return {};
      }
      const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      std::string out(static_cast<std::size_t>(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
      return out;
    }

    std::string executable_path() {
      std::wstring buffer(MAX_PATH, L'\0');
      for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length < buffer.size()) {
          buffer.resize(length);
          return narrow(buffer);
        }
        buffer.resize(buffer.size() * 2);
      }
    }

    std::string env(const char *name) {
      const char *value = std::getenv(name);
      return value ? value : "";
    }

    /** A PowerShell single-quoted string literal. */
    std::string ps_quote(const std::string &text) {
      std::string out = "'";
      for (char c : text) {
        out.push_back(c);
        if (c == '\'') {
          out.push_back('\'');
        }
      }
      out.push_back('\'');
      return out;
    }

    /** One argument, quoted for the Windows command line if needed. */
    std::string arg_quote(const std::string &arg) {
      if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
      }
      std::string out = "\"";
      for (char c : arg) {
        if (c == '"') {
          out += "\\\"";
        } else {
          out.push_back(c);
        }
      }
      out.push_back('"');
      return out;
    }

    /** Run a PowerShell script in this console, passed as -EncodedCommand to avoid quoting issues. */
    int run_powershell(const std::string &script) {
      const std::wstring wide = widen(script);
      static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string bytes;
      for (wchar_t c : wide) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
      }
      std::string encoded;
      for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned b2 = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        encoded.push_back(kAlphabet[(triple >> 18) & 63]);
        encoded.push_back(kAlphabet[(triple >> 12) & 63]);
        encoded.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 63] : '=');
        encoded.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 63] : '=');
      }
      const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encoded;
      std::fflush(stdout);
      std::fflush(stderr);
      return std::system(command.c_str());
    }

    const char *const kCommonScript =
      "$ErrorActionPreference = 'Stop'\n"
      "$name = 'Couchlink'\n"
      "$dir = Join-Path $env:ProgramFiles 'Couchlink'\n"
      "$exe = Join-Path $dir 'couchlink-host.exe'\n"
      "function Stop-Installed {\n"
      "  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue\n"
      "  Get-Process couchlink-host -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force\n"
      "  Start-Sleep -Milliseconds 500\n"
      "}\n";
  }  // namespace

  void show_pairing_code(const std::string &client_name, const std::string &code) {
    std::string spaced = code;
    if (spaced.size() == 6) {
      spaced.insert(3, " ");
    }
    const std::wstring text = widen(
      client_name + " wants to connect a Steam Controller to this PC with Couchlink.\n\n"
                    "Enter this code on it:\n\n"
                    "        " +
      spaced +
      "\n\n"
      "The code works for 2 minutes. If you did not ask for this, click OK and ignore it."
    );
    std::thread([text] {
      MessageBoxW(nullptr, text.c_str(), L"Couchlink", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }).detach();
  }

  void hide_console() {
    FreeConsole();
  }

  bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return false;
    }
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
  }

  int install(const InstallOptions &options) {
    if (!is_elevated()) {
      std::fprintf(stderr, "Run 'couchlink-host install' from an administrator terminal (Terminal (Admin)).\n");
      return 1;
    }

    const std::string log_dir = env("LOCALAPPDATA") + "\\Couchlink";
    std::string arguments = "run --hide-console --log " + arg_quote(log_dir + "\\couchlink-host.log");
    for (const auto &arg : options.run_arguments) {
      arguments += " " + arg_quote(arg);
    }

    std::string script = kCommonScript;
    script += "$source = " + ps_quote(executable_path()) + "\n";
    script += "$arguments = " + ps_quote(arguments) + "\n";
    script += "$port = " + std::to_string(options.port) + "\n";
    script +=
      "$user = \"$env:USERDOMAIN\\$env:USERNAME\"\n"
      "Stop-Installed\n"
      "New-Item -ItemType Directory -Force -Path $dir | Out-Null\n"
      "New-Item -ItemType Directory -Force -Path " +
      ps_quote(log_dir) +
      " | Out-Null\n"
      "if ((Resolve-Path $source).Path -ne $exe) { Copy-Item -Force $source $exe }\n"
      "Write-Host \"Installed to $exe\"\n"
      // Only the local network and Tailscale can reach the link port.
      "Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule\n"
      "New-NetFirewallRule -DisplayName $name -Direction Inbound -Protocol UDP -LocalPort $port -Program $exe "
      "-RemoteAddress LocalSubnet,100.64.0.0/10,fd7a:115c:a1e0::/48 -Profile Any -Action Allow | Out-Null\n"
      "Write-Host \"Firewall: UDP $port allowed from the local network and Tailscale\"\n"
      "$action = New-ScheduledTaskAction -Execute $exe -Argument $arguments -WorkingDirectory $dir\n"
      "$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user\n"
      "$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest\n"
      "$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries "
      "-DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew\n"
      "Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null\n"
      "Start-ScheduledTask -TaskName $name\n"
      "Write-Host \"Started. It will start by itself whenever $user logs on.\"\n";

    const int code = run_powershell(script);
    if (code != 0) {
      std::fprintf(stderr, "Install failed (PowerShell exit %d).\n", code);
      return 1;
    }
    std::printf(
      "\nAll set. Nothing else to run on this PC.\n"
      "To pair an iPad or iPhone: open Couchlink on it, enter this PC's address and tap Connect.\n"
      "A 6-digit code then pops up on this PC's screen; type it into Couchlink.\n"
      "Log: %s\\couchlink-host.log\n",
      log_dir.c_str()
    );
    return 0;
  }

  int uninstall() {
    if (!is_elevated()) {
      std::fprintf(stderr, "Run 'couchlink-host uninstall' from an administrator terminal (Terminal (Admin)).\n");
      return 1;
    }
    std::string script = kCommonScript;
    script +=
      "Stop-Installed\n"
      "Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue\n"
      "Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule\n"
      "Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue\n"
      "Write-Host 'Removed the logon task, the firewall rule and the installed copy. Paired devices are kept.'\n";
    return run_powershell(script) == 0 ? 0 : 1;
  }

#else

  void show_pairing_code(const std::string &, const std::string &) {}

  void hide_console() {}

  bool is_elevated() {
    return false;
  }

  int install(const InstallOptions &) {
    std::fprintf(stderr, "'install' is for Windows. On Linux, run 'couchlink-host run' as root from a systemd unit.\n");
    return 1;
  }

  int uninstall() {
    std::fprintf(stderr, "'uninstall' is for Windows.\n");
    return 1;
  }

#endif

}  // namespace couchlink::desktop
