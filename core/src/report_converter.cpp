#include "inputline/report_converter.h"

#include <cstring>

namespace inputline {

  bool StateReportConverter::to_wired(const std::uint8_t *report, std::size_t length, StateReport &out) {
    if (report == nullptr || length < 1) {
      return false;
    }

    TritonStateUsb state {};
    switch (report[0]) {
      case kReportState:
        if (length < kStateReportSize) {
          return false;
        }
        std::memcpy(out.data(), report, kStateReportSize);
        return true;

      case kReportStateBle: {
        if (length < kBleStateReportSize) {
          return false;
        }
        TritonStateBle ble;
        std::memcpy(&ble, report + 1, sizeof(ble));
        state.controls = ble.controls;
        state.pads = ble.pads;
        state.imu.timestamp_us = ble.imu.timestamp_us;
        std::memcpy(state.imu.accel, ble.imu.accel, sizeof(state.imu.accel));
        std::memcpy(state.imu.gyro, ble.imu.gyro, sizeof(state.imu.gyro));
        break;
      }

      case kReportStateTimestamped: {
        if (length < kBleStateReportSize) {
          return false;
        }
        TritonStateTimestamped ts;
        std::memcpy(&ts, report + 1, sizeof(ts));
        state.controls = ts.controls;
        state.pads = ts.pads;
        state.imu.timestamp_us = unwrap_timestamp(ts.imu.timestamp_32us);
        std::memcpy(state.imu.accel, ts.imu.accel, sizeof(state.imu.accel));
        std::memcpy(state.imu.gyro, ts.imu.gyro, sizeof(state.imu.gyro));
        break;
      }

      default:
        return false;
    }

    state.imu.quat[0] = kIdentityQuatW;
    out[0] = kReportState;
    std::memcpy(out.data() + 1, &state, sizeof(state));
    return true;
  }

  void StateReportConverter::reset() {
    have_timestamp_ = false;
    last_ticks_ = 0;
    timestamp_us_ = 0;
  }

  std::uint32_t StateReportConverter::unwrap_timestamp(std::uint16_t ticks_32us) {
    if (!have_timestamp_) {
      have_timestamp_ = true;
      timestamp_us_ = static_cast<std::uint32_t>(ticks_32us) * 32U;
    } else {
      // Unsigned subtraction handles the 16-bit wrap every ~2.1 s.
      const auto delta = static_cast<std::uint16_t>(ticks_32us - last_ticks_);
      timestamp_us_ += static_cast<std::uint32_t>(delta) * 32U;
    }
    last_ticks_ = ticks_32us;
    return timestamp_us_;
  }

}  // namespace inputline
