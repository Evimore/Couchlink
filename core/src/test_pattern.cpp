#include "couchlink/test_pattern.h"

#include <cmath>
#include <cstring>

namespace couchlink {

  std::array<std::uint8_t, kBleStateReportSize> TestPattern::frame(std::uint64_t elapsed_us) {
    constexpr double kTwoPi = 6.283185307179586;
    const double seconds = static_cast<double>(elapsed_us) / 1e6;
    const double phase = kTwoPi * seconds / 2.0;  // one revolution every 2 s

    TritonStateTimestamped state {};
    state.controls.seq = ++seq_;
    if (std::fmod(seconds, 1.0) < 0.25) {
      state.controls.buttons |= kButtonA;
    }
    state.controls.left_stick_x = static_cast<std::int16_t>(std::lround(20000.0 * std::cos(phase)));
    state.controls.left_stick_y = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(phase)));
    state.controls.trigger_right = static_cast<std::int16_t>(std::lround(16000.0 + 16000.0 * std::sin(phase)));

    state.controls.buttons |= kRightPadTouch;
    state.pads.right_x = static_cast<std::int16_t>(std::lround(12000.0 * std::cos(-phase)));
    state.pads.right_y = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(-phase)));
    state.pads.right_pressure = 8000;
    state.trackpad_timestamp = static_cast<std::uint16_t>(elapsed_us / 32);

    state.imu.timestamp_32us = static_cast<std::uint16_t>(elapsed_us / 32);
    state.imu.accel[2] = 16384;  // 1 g at the +-2 g scale
    state.imu.gyro[1] = static_cast<std::int16_t>(std::lround(800.0 * std::sin(phase)));

    std::array<std::uint8_t, kBleStateReportSize> report {};
    report[0] = kReportStateTimestamped;
    std::memcpy(report.data() + 1, &state, sizeof(state));
    return report;
  }

}  // namespace couchlink
