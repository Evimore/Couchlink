/**
 * @file version.h
 * @brief Comparing InputLine version numbers ("0.2.0", "v0.3.0-beta.1").
 *
 * The app and the PC use it to tell which side is older, and the PC to tell
 * whether a newer release is out.
 */
#pragma once

#include <string>

namespace inputline {

  /**
   * @brief Compare two versions by Semantic Versioning rules: numbers first,
   *        then a pre-release ("-beta.2") sorts before the release itself.
   *        A leading 'v' and build metadata ("+abc") are ignored.
   * @return Negative if a is older than b, 0 if equal, positive if newer.
   */
  int compare_versions(const std::string &a, const std::string &b);

  /** Whether the text looks like a version: digits, optionally with dots and a pre-release. */
  bool is_version(const std::string &text);

  /** Whether the version is a pre-release, such as "0.3.0-beta.1". */
  bool is_prerelease(const std::string &version);

}  // namespace inputline
