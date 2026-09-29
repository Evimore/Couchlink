/**
 * @file desktop.h
 * @brief The parts of inputline-host that touch the user's desktop: the pairing
 *        code popup, running in the background (as a Windows service or a
 *        logon task), and where it keeps its files.
 *
 * Everything here is Windows-specific; other platforms get harmless stubs.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace inputline::desktop {

  /** Name of the Windows service the installer sets up. */
  inline constexpr const char *kServiceName = "InputLine";

  /**
   * @brief Put a pairing code on the PC's screen without blocking the caller.
   *
   * The user types it into the InputLine app on the iPad or iPhone. It shows as
   * a Windows notification (from the service, on the signed-in user's screen),
   * or in a message box if that fails.
   */
  void show_pairing_code(const std::string &client_name, const std::string &code);

  /**
   * @brief The hidden 'notify TITLE TEXT SECONDS' command: show a Windows
   *        notification with the InputLine tray icon, and keep the icon for
   *        SECONDS. show_pairing_code() starts it in the user's session.
   * @return Process exit code.
   */
  int run_notifier();

  /** Detach from the console window (for the logon task). */
  void hide_console();

  /** Whether the process runs with administrator rights. */
  bool is_elevated();

  /**
   * @brief Where the background copy keeps its files: C:\ProgramData\InputLine
   *        on Windows (the log, options.txt, and pairings in its 'pairing'
   *        folder). Empty on other platforms.
   */
  std::string data_dir();

  /**
   * @brief Create data_dir() with safe permissions: only administrators and
   *        Windows can change it, users can read the log, and the pairing
   *        folder is readable by administrators and Windows only. Needs
   *        administrator rights. No-op elsewhere.
   */
  bool prepare_data_dirs();

  /** Whether the installer's InputLine service exists. */
  bool service_installed();

  /**
   * @brief Run as the Windows service: serve() runs until stop() makes it
   *        return, which happens when Windows stops the service.
   * @return Process exit code; 1 when not started by Windows.
   */
  int run_service(const std::function<int()> &serve, const std::function<void()> &stop);

  struct InstallOptions {
    /** Extra arguments for the installed 'run' command (port, config...). */
    std::vector<std::string> run_arguments;
    unsigned short port = 0;
  };

  /**
   * @brief Without the installer: copy this executable to Program Files,
   *        allow the link port through the firewall for the local network and
   *        Tailscale, and start it at every logon with administrator rights.
   * @return Process exit code.
   */
  int install(const InstallOptions &options);

  /** Undo install(). Paired devices are kept. */
  int uninstall();

}  // namespace inputline::desktop
