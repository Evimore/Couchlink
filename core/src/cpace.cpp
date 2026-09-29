#include "inputline/cpace.h"

#include "inputline/sha512.h"

#include <algorithm>
#include <cstring>

namespace inputline::cpace {

  namespace {
    const char kDsi[] = "CPace255";
    const char kDsiIsk[] = "CPace255_ISK";

    Bytes text(const char *value) {
      return Bytes(value, value + std::strlen(value));
    }

    Bytes bytes(const Point &point) {
      return Bytes(point.begin(), point.end());
    }
  }  // namespace

  Bytes prepend_len(const Bytes &data) {
    Bytes out;
    std::size_t length = data.size();
    do {
      std::uint8_t byte = static_cast<std::uint8_t>(length & 0x7f);
      length >>= 7;
      if (length != 0) {
        byte |= 0x80;
      }
      out.push_back(byte);
    } while (length != 0);
    out.insert(out.end(), data.begin(), data.end());
    return out;
  }

  Bytes lv_cat(const std::vector<Bytes> &parts) {
    Bytes out;
    for (const auto &part : parts) {
      const Bytes encoded = prepend_len(part);
      out.insert(out.end(), encoded.begin(), encoded.end());
    }
    return out;
  }

  Bytes generator_string(const Bytes &dsi, const Bytes &prs, const Bytes &ci, const Bytes &sid, std::size_t block_size) {
    const std::size_t used = prepend_len(prs).size() + prepend_len(dsi).size() + 1;
    const std::size_t zero_padding = block_size > used ? block_size - used : 0;
    return lv_cat({dsi, prs, Bytes(zero_padding, 0), ci, sid});
  }

  Point calculate_generator(const Bytes &prs, const Bytes &ci, const Bytes &sid) {
    Bytes generator = generator_string(text(kDsi), prs, ci, sid, Sha512::kBlockSize);
    const Sha512Digest hash = Sha512::hash(generator.data(), generator.size());
    std::fill(generator.begin(), generator.end(), 0);
    Point field_element {};
    std::copy(hash.begin(), hash.begin() + field_element.size(), field_element.begin());
    field_element[31] &= 0x7f;  // decodeUCoordinate for 255 bits
    const Point g = crypto::elligator2_curve25519(field_element);
    field_element.fill(0);
    return g;
  }

  Point public_share(const Scalar &scalar, const Point &generator) {
    Point share {};
    crypto::x25519(scalar, generator, share);
    return share;
  }

  std::optional<Isk> intermediate_key(
    const Scalar &scalar, const Point &peer_share, const Bytes &sid, const Point &ya, const Bytes &ada, const Point &yb, const Bytes &adb
  ) {
    Point k {};
    if (!crypto::x25519(scalar, peer_share, k)) {
      return std::nullopt;
    }
    Bytes input = lv_cat({text(kDsiIsk), sid, bytes(k)});
    const Bytes transcript = lv_cat({bytes(ya), ada, bytes(yb), adb});
    input.insert(input.end(), transcript.begin(), transcript.end());
    const Isk isk = Sha512::hash(input.data(), input.size());
    std::fill(input.begin(), input.end(), 0);
    k.fill(0);
    return isk;
  }

}  // namespace inputline::cpace
