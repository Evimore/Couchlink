/**
 * @file status_file.h
 * @brief What the service is doing, for the tray icon: a small text file in
 *        the data folder, which users can read (like the log).
 *
 * Format, one "key=value" per line:
 *   version=0.3.0
 *   time=<seconds since 1970, when written; the tray treats old files as "not running">
 *   controllers=1
 *   device=<name of a connected device>      (one line each)
 *   update=0.3.1
 *   update_url=https://github.com/...
 *   usbip=ok | missing | old        (usbip-win2)
 *   usbip_version=0.9.8.1
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inputline {

  struct ServiceStatus {
    std::string version;
    std::int64_t time = 0;
    std::size_t controllers = 0;
    std::vector<std::string> devices;
    std::string update_version;
    std::string update_url;
    std::string usbip = "ok";  ///< "ok", "missing" or "old"
    std::string usbip_version;
  };

  std::string format_status(const ServiceStatus &status);
  ServiceStatus parse_status(const std::string &text);

  /** Write @p status to @p path, replacing it in one step. */
  bool write_status_file(const std::string &path, const ServiceStatus &status);
  std::optional<ServiceStatus> read_status_file(const std::string &path);

}  // namespace inputline
