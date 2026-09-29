/**
 * @file crypto.h
 * @brief Small, dependency-free X25519 (RFC 7748) and ChaCha20-Poly1305
 *        (RFC 8439), for pairing and encrypting link datagrams.
 *
 * Written after the public-domain TweetNaCl and poly1305-donna designs:
 * constant-time, no tables indexed by secrets, no heap. Checked against the
 * RFC test vectors and an independent implementation in the tests.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inputline::crypto {

  using Key32 = std::array<std::uint8_t, 32>;
  using Nonce12 = std::array<std::uint8_t, 12>;
  using Tag16 = std::array<std::uint8_t, 16>;

  /** The public key for an X25519 secret key (32 random bytes, clamped internally). */
  Key32 x25519_public_key(const Key32 &secret);

  /**
   * @brief X25519 Diffie-Hellman.
   * @return false if the peer's key is a small-order point (the shared secret
   *         would be all zeros); `shared` must not be used then.
   */
  bool x25519(const Key32 &secret, const Key32 &peer_public, Key32 &shared);

  /** XOR `length` bytes with the ChaCha20 keystream starting at block `counter`. */
  void chacha20_xor(const Key32 &key, std::uint32_t counter, const Nonce12 &nonce, const std::uint8_t *in, std::uint8_t *out, std::size_t length);

  /** Poly1305 one-time authenticator. */
  Tag16 poly1305(const Key32 &one_time_key, const std::uint8_t *data, std::size_t length);

  /** ChaCha20-Poly1305 AEAD: encrypt `length` bytes (in place allowed) and authenticate them with `aad`. */
  Tag16 aead_seal(
    const Key32 &key, const Nonce12 &nonce, const std::uint8_t *aad, std::size_t aad_length,
    const std::uint8_t *plaintext, std::uint8_t *ciphertext, std::size_t length
  );

  /**
   * @brief Check the tag, then decrypt (in place allowed).
   * @return false if the tag does not match; nothing is written then.
   */
  bool aead_open(
    const Key32 &key, const Nonce12 &nonce, const std::uint8_t *aad, std::size_t aad_length,
    const std::uint8_t *ciphertext, std::uint8_t *plaintext, std::size_t length, const Tag16 &tag
  );

}  // namespace inputline::crypto
