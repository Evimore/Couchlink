/**
 * @file cpace.h
 * @brief CPace, a password-authenticated key exchange (PAKE), with the
 *        CPACE-X25519-SHA512 cipher suite of draft-irtf-cfrg-cpace, in the
 *        initiator-responder setting.
 *
 * Both sides derive a secret curve point (the generator) from the pairing
 * code, then run a Diffie-Hellman exchange on it. Someone in the middle who
 * doesn't know the code gets one guess per exchange; nothing on the wire lets
 * anyone test codes offline. Checked against the draft's test vectors.
 */
#pragma once

#include "crypto.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inputline::cpace {

  using Bytes = std::vector<std::uint8_t>;
  using Point = crypto::Key32;
  using Scalar = crypto::Key32;
  using Isk = std::array<std::uint8_t, 64>;

  /** The length of @p data in LEB128, then @p data. */
  Bytes prepend_len(const Bytes &data);

  /** prepend_len of each argument, concatenated. */
  Bytes lv_cat(const std::vector<Bytes> &parts);

  /** lv_cat(DSI, PRS, zero padding to fill the hash's first block, CI, sid). */
  Bytes generator_string(const Bytes &dsi, const Bytes &prs, const Bytes &ci, const Bytes &sid, std::size_t block_size);

  /** G.calculate_generator: the secret generator for this code and session. */
  Point calculate_generator(const Bytes &prs, const Bytes &ci, const Bytes &sid);

  /** This side's public share: X25519(scalar, generator). Scalars are 32 random bytes, never reused. */
  Point public_share(const Scalar &scalar, const Point &generator);

  /**
   * @brief The intermediate session key, ISK = SHA-512(lv_cat("CPace255_ISK",
   *        sid, K) || lv_cat(Ya, ADa) || lv_cat(Yb, ADb)), K = X25519(scalar, peer share).
   * @return nullopt if K is the neutral element (an invalid or low-order peer
   *         share): the exchange must be aborted.
   */
  std::optional<Isk> intermediate_key(
    const Scalar &scalar, const Point &peer_share, const Bytes &sid, const Point &ya, const Bytes &ada, const Point &yb, const Bytes &adb
  );

}  // namespace inputline::cpace
