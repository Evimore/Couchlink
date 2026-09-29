/**
 * @file test_pattern.h
 * @brief Synthetic controller frames for demos and end-to-end tests.
 *
 * Produces Bluetooth-format (0x47) state reports: the left stick and right
 * trackpad trace circles, A pulses once a second, and the gyro swings
 * gently, so each part of the pipeline shows visible motion in Steam's
 * controller test screen.
 */
#pragma once

#include "triton.h"

#include <array>
#include <cstdint>

namespace inputline {

  class TestPattern {
  public:
    /** Build the frame for @p elapsed_us microseconds after the pattern started. */
    std::array<std::uint8_t, kBleStateReportSize> frame(std::uint64_t elapsed_us);

  private:
    std::uint8_t seq_ = 0;
  };

}  // namespace inputline
