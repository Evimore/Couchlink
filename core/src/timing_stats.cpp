#include "inputline/timing_stats.h"

#include <algorithm>
#include <cstdio>

namespace inputline {

  void TimingStats::add(std::uint64_t now_us) {
    ++reports_;
    if (!have_last_) {
      have_last_ = true;
      last_us_ = now_us;
      return;
    }
    const std::uint64_t gap = now_us > last_us_ ? now_us - last_us_ : 0;
    last_us_ = now_us;
    ++gaps_;
    active_us_ += gap;
    max_gap_us_ = std::max(max_gap_us_, gap);
    over_20_ += gap > 20'000 ? 1 : 0;
    over_50_ += gap > 50'000 ? 1 : 0;
    over_100_ += gap > 100'000 ? 1 : 0;
    ++histogram_[std::min<std::uint64_t>(gap / kBucketUs, kBuckets)];
  }

  void TimingStats::break_sequence() {
    have_last_ = false;
  }

  TimingStats::Summary TimingStats::summary() const {
    Summary s;
    s.reports = reports_;
    s.seconds = static_cast<double>(active_us_) / 1e6;
    s.rate_hz = active_us_ > 0 ? static_cast<double>(gaps_) * 1e6 / static_cast<double>(active_us_) : 0;
    s.max_ms = static_cast<double>(max_gap_us_) / 1000.0;
    s.over_20ms = over_20_;
    s.over_50ms = over_50_;
    s.over_100ms = over_100_;

    auto percentile = [&](double fraction) {
      if (gaps_ == 0) {
        return 0.0;
      }
      const auto target = static_cast<std::uint64_t>(fraction * static_cast<double>(gaps_ - 1)) + 1;
      std::uint64_t seen = 0;
      for (std::size_t i = 0; i < histogram_.size(); ++i) {
        seen += histogram_[i];
        if (seen >= target) {
          if (i == kBuckets) {
            return s.max_ms;
          }
          // Lower edge of the bucket (0.25 ms resolution).
          return static_cast<double>(i * kBucketUs) / 1000.0;
        }
      }
      return s.max_ms;
    };
    s.p50_ms = percentile(0.50);
    s.p95_ms = percentile(0.95);
    s.p99_ms = percentile(0.99);
    return s;
  }

  void TimingStats::reset() {
    *this = TimingStats {};
  }

  std::string TimingStats::Summary::to_string() const {
    char text[200];
    std::snprintf(text, sizeof(text), "%.0f reports/s, gap p50 %.1f ms, p95 %.1f ms, p99 %.1f ms, max %.0f ms, >20 ms: %llu, >50 ms: %llu, >100 ms: %llu",
                  rate_hz, p50_ms, p95_ms, p99_ms, max_ms, static_cast<unsigned long long>(over_20ms),
                  static_cast<unsigned long long>(over_50ms), static_cast<unsigned long long>(over_100ms));
    return text;
  }

}  // namespace inputline
