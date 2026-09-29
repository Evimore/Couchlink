/**
 * @file timing_stats.h
 * @brief How regularly reports arrive: rate, gap percentiles and long gaps.
 *
 * Used on both ends of the link, so a measurement can tell whether jitter
 * comes from Bluetooth on the client or from the network.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace inputline {

  class TimingStats {
  public:
    struct Summary {
      std::uint64_t reports = 0;
      double seconds = 0;
      double rate_hz = 0;
      double p50_ms = 0;
      double p95_ms = 0;
      double p99_ms = 0;
      double max_ms = 0;
      std::uint64_t over_20ms = 0;
      std::uint64_t over_50ms = 0;
      std::uint64_t over_100ms = 0;

      /** e.g. "248 reports/s, gap p50 4.0 ms, p99 9.5 ms, max 120 ms, >20 ms: 3, >50 ms: 1, >100 ms: 1" */
      std::string to_string() const;
    };

    /** Record one report arriving at @p now_us (any monotonic microsecond clock). */
    void add(std::uint64_t now_us);

    /** Forget the gap before the next report (e.g. after the app was suspended on purpose). */
    void break_sequence();

    Summary summary() const;
    void reset();

  private:
    static constexpr std::uint64_t kBucketUs = 250;
    static constexpr std::size_t kBuckets = 1000;  // 0-250 ms, then overflow

    bool have_last_ = false;
    std::uint64_t last_us_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t max_gap_us_ = 0;
    std::uint64_t over_20_ = 0;
    std::uint64_t over_50_ = 0;
    std::uint64_t over_100_ = 0;
    std::uint64_t active_us_ = 0;  ///< time covered by counted gaps
    std::array<std::uint32_t, kBuckets + 1> histogram_ {};
  };

}  // namespace inputline
