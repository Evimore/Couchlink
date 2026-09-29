#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace couchlink::log {

  namespace {
    std::atomic<int> g_level {static_cast<int>(Level::kInfo)};
    std::mutex g_mutex;
    std::FILE *g_file = nullptr;

    const char *tag(Level level) {
      switch (level) {
        case Level::kDebug:
          return "debug";
        case Level::kInfo:
          return "info";
        case Level::kWarning:
          return "warn";
        case Level::kError:
          return "error";
      }
      return "?";
    }
  }  // namespace

  void set_level(Level level) {
    g_level = static_cast<int>(level);
  }

  bool set_file(const std::string &path) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    // Keep the previous run's log, for example from before a crash or restart.
    std::filesystem::rename(path, path + ".1", error);
    std::FILE *file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
      return false;
    }
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
      std::fclose(g_file);
    }
    g_file = file;
    return true;
  }

  bool enabled(Level level) {
    return static_cast<int>(level) >= g_level.load();
  }

  void write(Level level, const std::string &message) {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    const auto ms = duration_cast<milliseconds>(steady_clock::now() - start).count();

    std::lock_guard lock(g_mutex);
    std::FILE *out = g_file != nullptr ? g_file : stderr;
    std::fprintf(out, "[%8.3f] %-5s %s\n", static_cast<double>(ms) / 1000.0, tag(level), message.c_str());
    std::fflush(out);
  }

}  // namespace couchlink::log
