#include "status_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace inputline {

  namespace {
    /** Keep a value on one line. */
    std::string one_line(const std::string &value) {
      std::string out;
      for (char c : value) {
        out.push_back(c == '\n' || c == '\r' ? ' ' : c);
      }
      return out;
    }
  }  // namespace

  std::string format_status(const ServiceStatus &status) {
    std::ostringstream out;
    out << "version=" << one_line(status.version) << "\n";
    out << "time=" << status.time << "\n";
    out << "controllers=" << status.controllers << "\n";
    for (const auto &device : status.devices) {
      out << "device=" << one_line(device) << "\n";
    }
    if (!status.update_version.empty()) {
      out << "update=" << one_line(status.update_version) << "\n";
      out << "update_url=" << one_line(status.update_url) << "\n";
    }
    return out.str();
  }

  ServiceStatus parse_status(const std::string &text) {
    ServiceStatus status;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const auto equals = line.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      const std::string key = line.substr(0, equals);
      const std::string value = line.substr(equals + 1);
      if (key == "version") {
        status.version = value;
      } else if (key == "time") {
        status.time = std::strtoll(value.c_str(), nullptr, 10);
      } else if (key == "controllers") {
        status.controllers = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
      } else if (key == "device" && status.devices.size() < 16) {
        status.devices.push_back(value);
      } else if (key == "update") {
        status.update_version = value;
      } else if (key == "update_url") {
        status.update_url = value;
      }
    }
    return status;
  }

  bool write_status_file(const std::string &path, const ServiceStatus &status) {
    const std::string temporary = path + ".new";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      if (!out) {
        return false;
      }
      out << format_status(status);
      if (!out) {
        return false;
      }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
  }

  std::optional<ServiceStatus> read_status_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (text.str().size() > 64 * 1024) {
      return std::nullopt;
    }
    return parse_status(text.str());
  }

}  // namespace inputline
