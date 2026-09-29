/**
 * @file desktop.h
 * @brief The parts of couchlink-host that touch the user's desktop: the pairing
 *        code popup, running in the background, and installing as a logon task.
 *
 * Everything here is Windows-specific; other platforms get harmless stubs.
 */
#pragma once

#include <string>
#include <vector>

namespace couchlink::desktop {

  /**
   * @brief Put a pairing code on the PC's screen without blocking the caller.
   *
   * The user types it into the Couchlink app on the iPad or iPhone.
   */
  void show_pairing_code(const std::string &client_name, const std::string &code);

  /** Detach from the console window (for the logon task). */
  void hide_console();

  /** Whether the process runs with administrator rights. */
  bool is_elevated();

  struct InstallOptions {
    /** Extra arguments for the installed 'run' command (port, config...). */
    std::vector<std::string> run_arguments;
    unsigned short port = 0;
  };

  /**
   * @brief Copy this executable to Program Files, allow the link port through
   *        the firewall for the local network and Tailscale, and start it at
   *        every logon with administrator rights.
   * @return Process exit code.
   */
  int install(const InstallOptions &options);

  /** Undo install(). Paired devices are kept. */
  int uninstall();

}  // namespace couchlink::desktop
