#include "desktop.h"

#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
  #include <windows.h>

  #include <aclapi.h>
  #include <sddl.h>
  #include <shellapi.h>
  #include <wtsapi32.h>
#endif

namespace inputline::desktop {

#ifdef _WIN32

  namespace {
    std::atomic<bool> g_service_mode {false};

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

    /** One argument, quoted for the Windows command line if needed (CommandLineToArgvW rules). */
    std::string arg_quote(const std::string &arg) {
      if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
      }
      std::string out = "\"";
      std::size_t backslashes = 0;
      for (char c : arg) {
        if (c == '\\') {
          ++backslashes;
          continue;
        }
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
      }
      out.append(backslashes * 2, '\\');
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
      "$name = 'InputLine'\n"
      "$dir = Join-Path $env:ProgramFiles 'InputLine'\n"
      "$exe = Join-Path $dir 'inputline-host.exe'\n"
      "function Stop-Installed {\n"
      "  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue\n"
      "  Get-Process inputline-host -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force\n"
      "  Start-Sleep -Milliseconds 500\n"
      "}\n";
  }  // namespace

  namespace {
    constexpr UINT kNotifyIconMessage = WM_APP + 1;
    constexpr int kPairingSeconds = 120;  // how long a pairing code works

    /** The printable part of a name a device sent us, kept short. */
    std::string printable_name(const std::string &name) {
      std::string out;
      for (unsigned char c : name) {
        if (c >= 0x20 && c != 0x7F) {
          out.push_back(static_cast<char>(c));
        }
      }
      if (out.size() > 60) {
        out.resize(60);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
          out.pop_back();  // drop a cut-off UTF-8 sequence
        }
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) {
          out.pop_back();
        }
        out += "...";
      }
      return out.empty() ? "A device" : out;
    }

    /**
     * Start 'inputline-host notify' in the signed-in user's session (from the
     * service) or next to us, without waiting for it.
     */
    bool start_notifier(const std::string &title, const std::string &text) {
      const std::string exe = executable_path();
      std::wstring command_line = widen(
        arg_quote(exe) + " notify " + arg_quote(title) + " " + arg_quote(text) + " " + std::to_string(kPairingSeconds)
      );
      std::wstring desktop = L"winsta0\\default";
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      startup.lpDesktop = desktop.data();
      PROCESS_INFORMATION process {};
      BOOL started = FALSE;
      if (g_service_mode) {
        // A service has no desktop of its own: run the notifier as the user
        // who is signed in at the PC, in their session.
        const DWORD session = WTSGetActiveConsoleSessionId();
        HANDLE token = nullptr;
        if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) {
          log::debug("pairing: no signed-in user for a notification (error ", GetLastError(), ")");
          return false;
        }
        started = CreateProcessAsUserW(token, nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(token);
      } else {
        started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
      }
      if (!started) {
        log::debug("pairing: cannot start the notification (error ", GetLastError(), ")");
        return false;
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return true;
    }

    /** Window of the notifier: the tray icon's messages and its timer. */
    LRESULT CALLBACK notifier_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      if (message == WM_TIMER) {
        PostQuitMessage(0);
        return 0;
      }
      if (message == kNotifyIconMessage) {
        // Clicking the icon or the notification shows the code again.
        const UINT event = LOWORD(lparam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == NIN_BALLOONUSERCLICK) {
          const auto *text = reinterpret_cast<const std::wstring *>(GetWindowLongPtrW(window, GWLP_USERDATA));
          MessageBoxW(window, text->c_str(), L"InputLine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }
  }  // namespace

  void show_pairing_code(const std::string &client_name, const std::string &code) {
    std::string spaced = code;
    if (spaced.size() == 6) {
      spaced.insert(3, " ");
    }
    const std::string name = printable_name(client_name);
    // A Windows notification, like other background apps show. Windows
    // limits its title to 63 characters and its text to 255.
    const std::string title = "InputLine pairing code: " + spaced;
    const std::string text = name + " wants to connect a Steam Controller to this PC. Enter the code in InputLine on it. "
                                    "It works for 2 minutes. Didn't ask for this? Ignore it.";
    if (start_notifier(title, text)) {
      return;
    }

    // No notification: fall back to a message box.
    const std::wstring box = widen(
      name + " wants to connect a Steam Controller to this PC with InputLine.\n\n"
             "Enter this code on it:\n\n"
             "        " +
      spaced +
      "\n\n"
      "The code works for 2 minutes. If you did not ask for this, click OK and ignore it."
    );
    if (g_service_mode) {
      // Ask Windows to show it in the signed-in user's session, without waiting for OK.
      const DWORD session = WTSGetActiveConsoleSessionId();
      if (session == 0xFFFFFFFF) {
        log::warn("pairing: nobody is signed in to this PC to see the code");
        return;
      }
      std::wstring title_w = L"InputLine";
      std::wstring message = box;
      DWORD response = 0;
      if (!WTSSendMessageW(
            WTS_CURRENT_SERVER_HANDLE, session, title_w.data(), static_cast<DWORD>(title_w.size() * sizeof(wchar_t)),
            message.data(), static_cast<DWORD>(message.size() * sizeof(wchar_t)),
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND, kPairingSeconds, &response, FALSE
          )) {
        log::warn("pairing: could not show the code on screen (error ", GetLastError(), ")");
      }
      return;
    }
    std::thread([box] {
      MessageBoxW(nullptr, box.c_str(), L"InputLine", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }).detach();
  }

  int run_notifier() {
    // inputline-host notify TITLE TEXT SECONDS, read as UTF-16 so names in
    // any language survive.
    int count = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr || count < 4) {
      if (argv != nullptr) {
        LocalFree(argv);
      }
      return 2;
    }
    const std::wstring title = argv[2];
    std::wstring text = argv[3];
    const int seconds = count > 4 ? std::max(5, std::min(600, _wtoi(argv[4]))) : kPairingSeconds;
    LocalFree(argv);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class {};
    window_class.lpfnWndProc = notifier_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"InputLineNotifier";
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"InputLine", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
      return 1;
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&text));

    NOTIFYICONDATAW icon {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO | NIF_SHOWTIP;
    icon.uCallbackMessage = kNotifyIconMessage;
    const auto load_icon = [instance](int size_metric) {
      const int size = GetSystemMetrics(size_metric);
      return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    icon.hIcon = load_icon(SM_CXSMICON);
    icon.hBalloonIcon = load_icon(SM_CXICON);
    wcsncpy_s(icon.szTip, title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
    icon.dwInfoFlags = icon.hBalloonIcon != nullptr ? NIIF_USER | NIIF_LARGE_ICON : NIIF_INFO;
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
      DestroyWindow(window);
      return 1;
    }
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);

    // Keep the icon (and so the notification in the notification centre)
    // for as long as the code works.
    SetTimer(window, 1, static_cast<UINT>(seconds) * 1000, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &icon);
    DestroyWindow(window);
    for (HICON handle : {icon.hIcon, icon.hBalloonIcon}) {
      if (handle != nullptr) {
        DestroyIcon(handle);
      }
    }
    return 0;
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

  std::string data_dir() {
    const char *base = std::getenv("ProgramData");
    return std::string(base && *base ? base : "C:\\ProgramData") + "\\InputLine";
  }

  namespace {
    /**
     * Replace a folder's permissions with `sddl`, not inherited from above.
     * Files inside pick them up too.
     */
    bool secure_directory(const std::string &path, const wchar_t *sddl) {
      std::error_code error;
      std::filesystem::create_directories(path, error);
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
      }
      BOOL present = FALSE;
      BOOL defaulted = FALSE;
      PACL dacl = nullptr;
      bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present;
      if (ok) {
        std::wstring wide = widen(path);
        ok = SetNamedSecurityInfoW(
               wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
               nullptr, nullptr, dacl, nullptr
             ) == ERROR_SUCCESS;
      }
      LocalFree(descriptor);
      return ok;
    }

    // Windows (SYSTEM) and administrators: full control; users: read (the log).
    const wchar_t *const kDataDirSddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)";
    // Pairing keys: Windows and administrators only.
    const wchar_t *const kPairingDirSddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  }  // namespace

  bool prepare_data_dirs() {
    if (!is_elevated()) {
      return false;
    }
    const std::string dir = data_dir();
    const bool ok = secure_directory(dir, kDataDirSddl) && secure_directory(dir + "\\pairing", kPairingDirSddl);
    if (!ok) {
      log::warn("could not set the permissions of ", dir, " (error ", GetLastError(), ")");
    }
    return ok;
  }

  bool service_installed() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
      return false;
    }
    const std::wstring name = widen(kServiceName);
    SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_QUERY_STATUS);
    if (service != nullptr) {
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return service != nullptr;
  }

  namespace {
    std::function<int()> g_serve;
    std::function<void()> g_stop;
    SERVICE_STATUS_HANDLE g_status_handle = nullptr;
    SERVICE_STATUS g_status {};

    void report_status(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
      g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
      g_status.dwCurrentState = state;
      g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
      g_status.dwWin32ExitCode = exit_code == NO_ERROR ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
      g_status.dwServiceSpecificExitCode = exit_code;
      g_status.dwWaitHint = wait_hint;
      SetServiceStatus(g_status_handle, &g_status);
    }

    DWORD WINAPI service_control(DWORD control, DWORD, LPVOID, LPVOID) {
      switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
          report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
          g_stop();
          return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
          return NO_ERROR;
        default:
          return ERROR_CALL_NOT_IMPLEMENTED;
      }
    }

    /** Restart after a crash or a failed start, so the PC stays reachable. */
    void configure_recovery() {
      SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (manager == nullptr) {
        return;
      }
      const std::wstring name = widen(kServiceName);
      SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_START);
      if (service != nullptr) {
        SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 60000}};
        SERVICE_FAILURE_ACTIONSW failure {};
        failure.dwResetPeriod = 24 * 60 * 60;
        failure.cActions = 3;
        failure.lpsaActions = actions;
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
        SERVICE_FAILURE_ACTIONS_FLAG flag {};
        flag.fFailureActionsOnNonCrashFailures = TRUE;
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
        CloseServiceHandle(service);
      }
      CloseServiceHandle(manager);
    }

    void WINAPI service_main(DWORD, LPWSTR *) {
      const std::wstring name = widen(kServiceName);
      g_status_handle = RegisterServiceCtrlHandlerExW(name.c_str(), service_control, nullptr);
      if (g_status_handle == nullptr) {
        return;
      }
      report_status(SERVICE_START_PENDING, NO_ERROR, 5000);
      configure_recovery();
      report_status(SERVICE_RUNNING);
      const int code = g_serve();
      report_status(SERVICE_STOPPED, static_cast<DWORD>(code));
    }
  }  // namespace

  int run_service(const std::function<int()> &serve, const std::function<void()> &stop) {
    g_serve = serve;
    g_stop = stop;
    g_service_mode = true;
    std::wstring name = widen(kServiceName);
    SERVICE_TABLE_ENTRYW table[] = {{name.data(), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
      g_service_mode = false;
      if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        std::fprintf(stderr, "'inputline-host service' is started by Windows (the InputLine installer sets it up). In a terminal, use 'inputline-host run'.\n");
      }
      return 1;
    }
    return 0;
  }

  int install(const InstallOptions &options) {
    if (service_installed()) {
      std::fprintf(stderr, "InputLine is installed with its installer and already runs in the background. Nothing to do.\n");
      return 1;
    }
    if (!is_elevated()) {
      std::fprintf(stderr, "Run 'inputline-host install' from an administrator terminal (Terminal (Admin)).\n");
      return 1;
    }

    prepare_data_dirs();
    const std::string log_dir = data_dir();
    std::string arguments = "run --hide-console --log " + arg_quote(log_dir + "\\inputline-host.log");
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
      "To pair an iPad or iPhone: open InputLine on it, enter this PC's address and tap Connect.\n"
      "A 6-digit code then pops up on this PC's screen; type it into InputLine.\n"
      "Log: %s\\inputline-host.log\n",
      log_dir.c_str()
    );
    return 0;
  }

  int uninstall() {
    if (service_installed()) {
      std::fprintf(stderr, "InputLine was installed with its installer: remove it in Settings > Apps > Installed apps.\n");
      return 1;
    }
    if (!is_elevated()) {
      std::fprintf(stderr, "Run 'inputline-host uninstall' from an administrator terminal (Terminal (Admin)).\n");
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

  int run_notifier() {
    return 1;
  }

  void hide_console() {}

  bool is_elevated() {
    return false;
  }

  std::string data_dir() {
    return {};
  }

  bool prepare_data_dirs() {
    return false;
  }

  bool service_installed() {
    return false;
  }

  int run_service(const std::function<int()> &, const std::function<void()> &) {
    std::fprintf(stderr, "'service' is for Windows. On Linux, run 'inputline-host run' from a systemd unit.\n");
    return 1;
  }

  int install(const InstallOptions &) {
    std::fprintf(stderr, "'install' is for Windows. On Linux, run 'inputline-host run' as root from a systemd unit.\n");
    return 1;
  }

  int uninstall() {
    std::fprintf(stderr, "'uninstall' is for Windows.\n");
    return 1;
  }

#endif

}  // namespace inputline::desktop
