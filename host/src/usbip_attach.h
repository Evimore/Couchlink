/**
 * @file usbip_attach.h
 * @brief Ask the operating system's USB/IP client to attach an exported device.
 *
 * Windows: usbip-win2's usbip.exe. Linux: the usbip tool from linux-tools,
 * with the vhci-hcd module loaded. Both need administrator/root rights.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inputline {

  struct AttachOptions {
    bool enabled = true;
    std::string executable;  ///< Empty: use default_usbip_executable().
    std::string host = "127.0.0.1";
    std::uint16_t port = 3240;
    /** Extra arguments for newer usbip-win2 releases; retried without them if rejected. */
    bool use_low_latency_mode = true;
  };

  /** Where usbip-win2 installs its CLI, or plain "usbip" on Linux. */
  std::string default_usbip_executable();

  /** Build the argument vector (argv[0] included) for an attach call. */
  std::vector<std::string> attach_command(const AttachOptions &options, const std::string &busid, bool with_extras);

  /** Run the attach command; blocks until it exits. Returns true on exit code 0. */
  bool run_usbip_attach(const AttachOptions &options, const std::string &busid);

  /**
   * @brief Run a program with arguments (no shell).
   * @param output If given, receives what the program wrote to stdout and stderr.
   * @return Its exit code, or -1 if it could not start.
   */
  int run_process(const std::vector<std::string> &argv, std::string *output = nullptr);

}  // namespace inputline
