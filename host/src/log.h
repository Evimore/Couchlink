/**
 * @file log.h
 * @brief Minimal thread-safe logging to stderr.
 */
#pragma once

#include <sstream>
#include <string>

namespace inputline::log {

  enum class Level {
    kDebug = 0,
    kInfo = 1,
    kWarning = 2,
    kError = 3,
  };

  void set_level(Level level);
  /** Write to this file instead of stderr, e.g. when running without a console. The previous one is kept as PATH.1. */
  bool set_file(const std::string &path);
  bool enabled(Level level);
  void write(Level level, const std::string &message);

  template<typename... Args>
  void emit(Level level, const Args &...args) {
    if (!enabled(level)) {
      return;
    }
    std::ostringstream stream;
    (stream << ... << args);
    write(level, stream.str());
  }

  template<typename... Args>
  void debug(const Args &...args) {
    emit(Level::kDebug, args...);
  }

  template<typename... Args>
  void info(const Args &...args) {
    emit(Level::kInfo, args...);
  }

  template<typename... Args>
  void warn(const Args &...args) {
    emit(Level::kWarning, args...);
  }

  template<typename... Args>
  void error(const Args &...args) {
    emit(Level::kError, args...);
  }

}  // namespace inputline::log
