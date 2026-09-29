/**
 * @file sha512.h
 * @brief Small, dependency-free SHA-512 (FIPS 180-4), for CPace pairing.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inputline {

  using Sha512Digest = std::array<std::uint8_t, 64>;

  class Sha512 {
  public:
    /** Input block size in bytes (CPace pads its generator string to it). */
    static constexpr std::size_t kBlockSize = 128;

    Sha512();
    void update(const void *data, std::size_t length);
    Sha512Digest finish();

    static Sha512Digest hash(const void *data, std::size_t length);

  private:
    void compress(const std::uint8_t *block);

    std::array<std::uint64_t, 8> state_;
    std::array<std::uint8_t, kBlockSize> buffer_ {};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
  };

}  // namespace inputline
