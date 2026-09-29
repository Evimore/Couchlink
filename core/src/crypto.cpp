#include "inputline/crypto.h"

#include "inputline/sha256.h"

#include <algorithm>
#include <cstring>

namespace inputline::crypto {

  namespace {

    // ---- X25519: field arithmetic mod 2^255 - 19 in 16 limbs of 16 bits ----

    using Field = std::array<std::int64_t, 16>;

    constexpr Field k121665 = {0xDB41, 1};

    void carry(Field &o) {
      for (int i = 0; i < 16; ++i) {
        o[i] += std::int64_t {1} << 16;
        const std::int64_t c = o[i] >> 16;
        if (i < 15) {
          o[i + 1] += c - 1;
        } else {
          o[0] += 38 * (c - 1);
        }
        o[i] -= c * 65536;
      }
    }

    /** Swap p and q when b is 1, in constant time. */
    void conditional_swap(Field &p, Field &q, std::int64_t b) {
      const std::int64_t mask = ~(b - 1);
      for (int i = 0; i < 16; ++i) {
        const std::int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
      }
    }

    void pack(std::uint8_t *out, const Field &n) {
      Field t = n;
      carry(t);
      carry(t);
      carry(t);
      Field m {};
      for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
          m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
          m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const std::int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        conditional_swap(t, m, 1 - b);
      }
      for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
        out[2 * i + 1] = static_cast<std::uint8_t>((t[i] >> 8) & 0xff);
      }
    }

    void unpack(Field &o, const std::uint8_t *n) {
      for (int i = 0; i < 16; ++i) {
        o[i] = n[2 * i] + (std::int64_t {n[2 * i + 1]} << 8);
      }
      o[15] &= 0x7fff;
    }

    void add(Field &o, const Field &a, const Field &b) {
      for (int i = 0; i < 16; ++i) {
        o[i] = a[i] + b[i];
      }
    }

    void sub(Field &o, const Field &a, const Field &b) {
      for (int i = 0; i < 16; ++i) {
        o[i] = a[i] - b[i];
      }
    }

    void mul(Field &o, const Field &a, const Field &b) {
      std::int64_t t[31] = {};
      for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
          t[i + j] += a[i] * b[j];
        }
      }
      for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
      }
      for (int i = 0; i < 16; ++i) {
        o[i] = t[i];
      }
      carry(o);
      carry(o);
    }

    void square(Field &o, const Field &a) {
      mul(o, a, a);
    }

    /** o = i^(p-2), the inverse mod p. */
    void invert(Field &o, const Field &in) {
      Field c = in;
      for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4) {
          mul(c, c, in);
        }
      }
      o = c;
    }

    void scalar_mult(std::uint8_t *q, const std::uint8_t *n, const std::uint8_t *p) {
      std::uint8_t z[32];
      std::memcpy(z, n, 32);
      z[31] = static_cast<std::uint8_t>((n[31] & 127) | 64);
      z[0] &= 248;

      Field x {};
      unpack(x, p);
      Field a {}, b = x, c {}, d {}, e {}, f {};
      a[0] = d[0] = 1;
      for (int i = 254; i >= 0; --i) {
        const std::int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        conditional_swap(a, b, r);
        conditional_swap(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        square(d, e);
        square(f, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        square(b, a);
        sub(c, d, f);
        mul(a, c, k121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        square(b, e);
        conditional_swap(a, b, r);
        conditional_swap(c, d, r);
      }
      invert(c, c);
      mul(a, a, c);
      pack(q, a);
      std::memset(z, 0, sizeof(z));
    }

    // ---- ChaCha20 ----

    std::uint32_t load32(const std::uint8_t *p) {
      return std::uint32_t {p[0]} | (std::uint32_t {p[1]} << 8) | (std::uint32_t {p[2]} << 16) | (std::uint32_t {p[3]} << 24);
    }

    void store32(std::uint8_t *p, std::uint32_t v) {
      p[0] = static_cast<std::uint8_t>(v);
      p[1] = static_cast<std::uint8_t>(v >> 8);
      p[2] = static_cast<std::uint8_t>(v >> 16);
      p[3] = static_cast<std::uint8_t>(v >> 24);
    }

    void store64(std::uint8_t *p, std::uint64_t v) {
      store32(p, static_cast<std::uint32_t>(v));
      store32(p + 4, static_cast<std::uint32_t>(v >> 32));
    }

    constexpr std::uint32_t rotl(std::uint32_t x, int n) {
      return (x << n) | (x >> (32 - n));
    }

    void quarter_round(std::uint32_t &a, std::uint32_t &b, std::uint32_t &c, std::uint32_t &d) {
      a += b;
      d = rotl(d ^ a, 16);
      c += d;
      b = rotl(b ^ c, 12);
      a += b;
      d = rotl(d ^ a, 8);
      c += d;
      b = rotl(b ^ c, 7);
    }

    void chacha20_block(const Key32 &key, std::uint32_t counter, const Nonce12 &nonce, std::uint8_t out[64]) {
      std::uint32_t state[16] = {
        0x61707865,
        0x3320646e,
        0x79622d32,
        0x6b206574,
        load32(&key[0]),
        load32(&key[4]),
        load32(&key[8]),
        load32(&key[12]),
        load32(&key[16]),
        load32(&key[20]),
        load32(&key[24]),
        load32(&key[28]),
        counter,
        load32(&nonce[0]),
        load32(&nonce[4]),
        load32(&nonce[8]),
      };
      std::uint32_t x[16];
      std::memcpy(x, state, sizeof(x));
      for (int i = 0; i < 10; ++i) {
        quarter_round(x[0], x[4], x[8], x[12]);
        quarter_round(x[1], x[5], x[9], x[13]);
        quarter_round(x[2], x[6], x[10], x[14]);
        quarter_round(x[3], x[7], x[11], x[15]);
        quarter_round(x[0], x[5], x[10], x[15]);
        quarter_round(x[1], x[6], x[11], x[12]);
        quarter_round(x[2], x[7], x[8], x[13]);
        quarter_round(x[3], x[4], x[9], x[14]);
      }
      for (int i = 0; i < 16; ++i) {
        store32(out + 4 * i, x[i] + state[i]);
      }
    }

    // ---- Poly1305, 26-bit limbs ----

    class Poly1305 {
    public:
      explicit Poly1305(const Key32 &key) {
        r_[0] = load32(&key[0]) & 0x3ffffff;
        r_[1] = (load32(&key[3]) >> 2) & 0x3ffff03;
        r_[2] = (load32(&key[6]) >> 4) & 0x3ffc0ff;
        r_[3] = (load32(&key[9]) >> 6) & 0x3f03fff;
        r_[4] = (load32(&key[12]) >> 8) & 0x00fffff;
        for (int i = 0; i < 4; ++i) {
          pad_[i] = load32(&key[16 + 4 * i]);
        }
      }

      void update(const std::uint8_t *data, std::size_t length) {
        while (length > 0) {
          const std::size_t take = std::min<std::size_t>(16 - buffered_, length);
          std::memcpy(buffer_ + buffered_, data, take);
          buffered_ += take;
          data += take;
          length -= take;
          if (buffered_ == 16) {
            block(buffer_, 1u << 24);
            buffered_ = 0;
          }
        }
      }

      /** Zero-pad to a 16-byte boundary (the AEAD's padding). */
      void pad16() {
        if (buffered_ != 0) {
          std::memset(buffer_ + buffered_, 0, 16 - buffered_);
          block(buffer_, 1u << 24);
          buffered_ = 0;
        }
      }

      Tag16 finish() {
        if (buffered_ != 0) {
          buffer_[buffered_] = 1;
          std::memset(buffer_ + buffered_ + 1, 0, 16 - buffered_ - 1);
          block(buffer_, 0);
        }
        std::uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];
        std::uint32_t c = h1 >> 26;
        h1 &= 0x3ffffff;
        h2 += c;
        c = h2 >> 26;
        h2 &= 0x3ffffff;
        h3 += c;
        c = h3 >> 26;
        h3 &= 0x3ffffff;
        h4 += c;
        c = h4 >> 26;
        h4 &= 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;

        // g = h + 5 - 2^130; use it if h >= p.
        std::uint32_t g0 = h0 + 5;
        c = g0 >> 26;
        g0 &= 0x3ffffff;
        std::uint32_t g1 = h1 + c;
        c = g1 >> 26;
        g1 &= 0x3ffffff;
        std::uint32_t g2 = h2 + c;
        c = g2 >> 26;
        g2 &= 0x3ffffff;
        std::uint32_t g3 = h3 + c;
        c = g3 >> 26;
        g3 &= 0x3ffffff;
        const std::uint32_t g4 = h4 + c - (1u << 26);

        std::uint32_t mask = (g4 >> 31) - 1;
        g0 &= mask;
        g1 &= mask;
        g2 &= mask;
        g3 &= mask;
        const std::uint32_t g4m = g4 & mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0;
        h1 = (h1 & mask) | g1;
        h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3;
        h4 = (h4 & mask) | g4m;

        const std::uint32_t w0 = h0 | (h1 << 26);
        const std::uint32_t w1 = (h1 >> 6) | (h2 << 20);
        const std::uint32_t w2 = (h2 >> 12) | (h3 << 14);
        const std::uint32_t w3 = (h3 >> 18) | (h4 << 8);

        Tag16 tag {};
        std::uint64_t f = std::uint64_t {w0} + pad_[0];
        store32(&tag[0], static_cast<std::uint32_t>(f));
        f = std::uint64_t {w1} + pad_[1] + (f >> 32);
        store32(&tag[4], static_cast<std::uint32_t>(f));
        f = std::uint64_t {w2} + pad_[2] + (f >> 32);
        store32(&tag[8], static_cast<std::uint32_t>(f));
        f = std::uint64_t {w3} + pad_[3] + (f >> 32);
        store32(&tag[12], static_cast<std::uint32_t>(f));
        return tag;
      }

    private:
      void block(const std::uint8_t *m, std::uint32_t hibit) {
        const std::uint64_t r0 = r_[0], r1 = r_[1], r2 = r_[2], r3 = r_[3], r4 = r_[4];
        const std::uint64_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        std::uint64_t h0 = h_[0] + (load32(m) & 0x3ffffff);
        std::uint64_t h1 = h_[1] + ((load32(m + 3) >> 2) & 0x3ffffff);
        std::uint64_t h2 = h_[2] + ((load32(m + 6) >> 4) & 0x3ffffff);
        std::uint64_t h3 = h_[3] + ((load32(m + 9) >> 6) & 0x3ffffff);
        std::uint64_t h4 = h_[4] + ((load32(m + 12) >> 8) | hibit);

        const std::uint64_t d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
        std::uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
        std::uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
        std::uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
        std::uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;

        std::uint64_t c = d0 >> 26;
        h0 = d0 & 0x3ffffff;
        d1 += c;
        c = d1 >> 26;
        h1 = d1 & 0x3ffffff;
        d2 += c;
        c = d2 >> 26;
        h2 = d2 & 0x3ffffff;
        d3 += c;
        c = d3 >> 26;
        h3 = d3 & 0x3ffffff;
        d4 += c;
        c = d4 >> 26;
        h4 = d4 & 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;

        h_[0] = static_cast<std::uint32_t>(h0);
        h_[1] = static_cast<std::uint32_t>(h1);
        h_[2] = static_cast<std::uint32_t>(h2);
        h_[3] = static_cast<std::uint32_t>(h3);
        h_[4] = static_cast<std::uint32_t>(h4);
      }

      std::uint32_t r_[5] {};
      std::uint32_t h_[5] {};
      std::uint32_t pad_[4] {};
      std::uint8_t buffer_[16] {};
      std::size_t buffered_ = 0;
    };

    Tag16 aead_tag(const Key32 &key, const Nonce12 &nonce, const std::uint8_t *aad, std::size_t aad_length, const std::uint8_t *ciphertext, std::size_t length) {
      std::uint8_t block[64];
      chacha20_block(key, 0, nonce, block);
      Key32 one_time {};
      std::memcpy(one_time.data(), block, 32);
      Poly1305 mac(one_time);
      mac.update(aad, aad_length);
      mac.pad16();
      mac.update(ciphertext, length);
      mac.pad16();
      std::uint8_t lengths[16];
      store64(lengths, aad_length);
      store64(lengths + 8, length);
      mac.update(lengths, sizeof(lengths));
      std::memset(block, 0, sizeof(block));
      std::memset(one_time.data(), 0, one_time.size());
      return mac.finish();
    }

  }  // namespace

  Key32 x25519_public_key(const Key32 &secret) {
    static const std::uint8_t kBasePoint[32] = {9};
    Key32 out {};
    scalar_mult(out.data(), secret.data(), kBasePoint);
    return out;
  }

  bool x25519(const Key32 &secret, const Key32 &peer_public, Key32 &shared) {
    scalar_mult(shared.data(), secret.data(), peer_public.data());
    std::uint8_t any = 0;
    for (std::uint8_t b : shared) {
      any |= b;
    }
    return any != 0;
  }

  void chacha20_xor(const Key32 &key, std::uint32_t counter, const Nonce12 &nonce, const std::uint8_t *in, std::uint8_t *out, std::size_t length) {
    std::uint8_t block[64];
    while (length > 0) {
      chacha20_block(key, counter++, nonce, block);
      const std::size_t take = std::min<std::size_t>(64, length);
      for (std::size_t i = 0; i < take; ++i) {
        out[i] = static_cast<std::uint8_t>(in[i] ^ block[i]);
      }
      in += take;
      out += take;
      length -= take;
    }
    std::memset(block, 0, sizeof(block));
  }

  Tag16 poly1305(const Key32 &one_time_key, const std::uint8_t *data, std::size_t length) {
    Poly1305 mac(one_time_key);
    mac.update(data, length);
    return mac.finish();
  }

  Tag16 aead_seal(
    const Key32 &key, const Nonce12 &nonce, const std::uint8_t *aad, std::size_t aad_length,
    const std::uint8_t *plaintext, std::uint8_t *ciphertext, std::size_t length
  ) {
    chacha20_xor(key, 1, nonce, plaintext, ciphertext, length);
    return aead_tag(key, nonce, aad, aad_length, ciphertext, length);
  }

  bool aead_open(
    const Key32 &key, const Nonce12 &nonce, const std::uint8_t *aad, std::size_t aad_length,
    const std::uint8_t *ciphertext, std::uint8_t *plaintext, std::size_t length, const Tag16 &tag
  ) {
    const Tag16 expected = aead_tag(key, nonce, aad, aad_length, ciphertext, length);
    if (!constant_time_equal(expected.data(), tag.data(), tag.size())) {
      return false;
    }
    chacha20_xor(key, 1, nonce, ciphertext, plaintext, length);
    return true;
  }

}  // namespace inputline::crypto
