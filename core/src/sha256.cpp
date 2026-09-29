#include "couchlink/sha256.h"

#include <algorithm>
#include <cstring>

namespace couchlink {

  namespace {
    constexpr std::array<std::uint32_t, 64> kRound = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    constexpr std::uint32_t rotr(std::uint32_t x, int n) {
      return (x >> n) | (x << (32 - n));
    }
  }  // namespace

  Sha256::Sha256():
      state_ {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

  void Sha256::compress(const std::uint8_t *block) {
    std::array<std::uint32_t, 64> w {};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
      const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto ch = (e & f) ^ (~e & g);
      const auto t1 = h + s1 + ch + kRound[i] + w[i];
      const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  void Sha256::update(const void *data, std::size_t length) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    total_bytes_ += length;
    while (length > 0) {
      const auto take = std::min(length, buffer_.size() - buffered_);
      std::memcpy(buffer_.data() + buffered_, bytes, take);
      buffered_ += take;
      bytes += take;
      length -= take;
      if (buffered_ == buffer_.size()) {
        compress(buffer_.data());
        buffered_ = 0;
      }
    }
  }

  Sha256Digest Sha256::finish() {
    const std::uint64_t bit_length = total_bytes_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (buffered_ != 56) {
      update(&zero, 1);
    }
    std::uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56 - 8 * i));
    }
    update(length_bytes, sizeof(length_bytes));

    Sha256Digest digest {};
    for (int i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
      digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
      digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
      digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
  }

  Sha256Digest Sha256::hash(const void *data, std::size_t length) {
    Sha256 sha;
    sha.update(data, length);
    return sha.finish();
  }

  HmacSha256::HmacSha256(const void *key, std::size_t key_length) {
    std::array<std::uint8_t, 64> block_key {};
    if (key_length > block_key.size()) {
      const auto digest = Sha256::hash(key, key_length);
      std::memcpy(block_key.data(), digest.data(), digest.size());
    } else if (key_length > 0) {
      std::memcpy(block_key.data(), key, key_length);
    }

    std::array<std::uint8_t, 64> inner_pad {};
    for (std::size_t i = 0; i < block_key.size(); ++i) {
      inner_pad[i] = static_cast<std::uint8_t>(block_key[i] ^ 0x36);
      outer_key_pad_[i] = static_cast<std::uint8_t>(block_key[i] ^ 0x5c);
    }
    inner_.update(inner_pad.data(), inner_pad.size());
  }

  void HmacSha256::update(const void *data, std::size_t length) {
    inner_.update(data, length);
  }

  Sha256Digest HmacSha256::finish() {
    const auto inner_digest = inner_.finish();
    Sha256 outer;
    outer.update(outer_key_pad_.data(), outer_key_pad_.size());
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
  }

  Sha256Digest HmacSha256::mac(const void *key, std::size_t key_length, const void *data, std::size_t length) {
    HmacSha256 hmac(key, key_length);
    hmac.update(data, length);
    return hmac.finish();
  }

  bool constant_time_equal(const std::uint8_t *a, const std::uint8_t *b, std::size_t length) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < length; ++i) {
      difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
    }
    return difference == 0;
  }

}  // namespace couchlink
