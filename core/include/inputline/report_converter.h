/**
 * @file report_converter.h
 * @brief Converts Bluetooth state reports into the wired report Steam expects.
 *
 * The client reads 0x45 or 0x47 frames over Bluetooth LE. The host presents a
 * wired 28DE:1302 controller to Steam, whose native state report is 0x42. This
 * class repacks one into the other and keeps a continuous IMU clock so Steam
 * Input's gyro integration sees real sample spacing.
 */
#pragma once

#include "triton.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace inputline {

  using StateReport = std::array<std::uint8_t, kStateReportSize>;

  /**
   * Quaternion written when the source report has none (Bluetooth frames).
   * Identity orientation at Q15 scale. Whether Steam Input reads this field
   * from a 28DE:1302 device is an open question (see PLAN.md, Phase 1).
   */
  constexpr std::int16_t kIdentityQuatW = 32767;

  class StateReportConverter {
  public:
    /**
     * @brief Repack one state report into wired report 0x42.
     * @param report Report bytes including the leading report ID.
     * @param length Number of valid bytes in @p report.
     * @param out Receives the 54-byte report 0x42, ID included.
     * @return false when @p report is not a complete 0x42, 0x45 or 0x47 frame.
     */
    bool to_wired(const std::uint8_t *report, std::size_t length, StateReport &out);

    /** Forget IMU clock history, e.g. after the controller reconnects. */
    void reset();

  private:
    std::uint32_t unwrap_timestamp(std::uint16_t ticks_32us);

    bool have_timestamp_ = false;
    std::uint16_t last_ticks_ = 0;
    std::uint32_t timestamp_us_ = 0;
  };

}  // namespace inputline
