/**
 * @file sha256.h
 * @brief Small, dependency-free SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104).
 *
 * Used to authenticate link datagrams on platforms where pulling in OpenSSL
 * or CommonCrypto would be the only reason for a dependency.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace couchlink {

  using Sha256Digest = std::array<std::uint8_t, 32>;

  class Sha256 {
  public:
    Sha256();
    void update(const void *data, std::size_t length);
    Sha256Digest finish();

    static Sha256Digest hash(const void *data, std::size_t length);

  private:
    void compress(const std::uint8_t *block);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_ {};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
  };

  class HmacSha256 {
  public:
    HmacSha256(const void *key, std::size_t key_length);
    void update(const void *data, std::size_t length);
    Sha256Digest finish();

    static Sha256Digest mac(const void *key, std::size_t key_length, const void *data, std::size_t length);

  private:
    Sha256 inner_;
    std::array<std::uint8_t, 64> outer_key_pad_ {};
  };

  /** Constant-time comparison of two equal-length buffers. */
  bool constant_time_equal(const std::uint8_t *a, const std::uint8_t *b, std::size_t length);

}  // namespace couchlink
