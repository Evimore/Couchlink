/**
 * @file check.h
 * @brief Tiny assertion helpers shared by the host test programs.
 */
#pragma once

#include <cstdio>

namespace couchlink::test {
  inline int g_failures = 0;
  inline int g_checks = 0;

  inline int report_and_exit_code() {
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
  }
}  // namespace couchlink::test

#define CHECK(cond)                                                   \
  do {                                                                \
    ++::couchlink::test::g_checks;                                       \
    if (!(cond)) {                                                    \
      ++::couchlink::test::g_failures;                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                 \
  } while (0)
