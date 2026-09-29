// Tests for X25519 and ChaCha20-Poly1305: RFC 7748 and RFC 8439 vectors, and
// vectors from an independent implementation (Python's cryptography package)
// covering every padding case of the AEAD.

#include "inputline/crypto.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace inputline::crypto;

namespace {

  int g_checks = 0;
  int g_failures = 0;

#define CHECK(condition) \
  do { \
    ++g_checks; \
    if (!(condition)) { \
      ++g_failures; \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
    } \
  } while (0)

  std::vector<std::uint8_t> hex(const std::string &text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
      out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return out;
  }

  template<std::size_t N>
  std::array<std::uint8_t, N> fixed(const std::string &text) {
    std::array<std::uint8_t, N> out {};
    const auto bytes = hex(text);
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
  }

  template<std::size_t N>
  bool same(const std::array<std::uint8_t, N> &a, const std::string &text) {
    return std::vector<std::uint8_t>(a.begin(), a.end()) == hex(text);
  }

  void test_x25519() {
    // RFC 7748 section 5.2.
    Key32 shared {};
    CHECK(x25519(fixed<32>("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                 fixed<32>("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"), shared));
    CHECK(same(shared, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

    // RFC 7748 section 6.1: both sides agree.
    const auto alice = fixed<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bob = fixed<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    CHECK(same(x25519_public_key(alice), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK(same(x25519_public_key(bob), "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    Key32 a {}, b {};
    CHECK(x25519(alice, x25519_public_key(bob), a));
    CHECK(x25519(bob, x25519_public_key(alice), b));
    CHECK(a == b);
    CHECK(same(a, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));

    // A small-order peer key would give an all-zero secret: refused.
    Key32 zero_point {};
    CHECK(!x25519(alice, zero_point, shared));
    Key32 one_point {};
    one_point[0] = 1;
    CHECK(!x25519(alice, one_point, shared));
  }

  void test_poly1305() {
    // RFC 8439 section 2.5.2.
    const std::string message = "Cryptographic Forum Research Group";
    const auto tag = poly1305(fixed<32>("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
                              reinterpret_cast<const std::uint8_t *>(message.data()), message.size());
    CHECK(same(tag, "a8061dc1305136c6c22b8baf0c0127a9"));
  }

  void check_aead(const std::string &key_hex, const std::string &nonce_hex, const std::string &aad_hex,
                  const std::vector<std::uint8_t> &plaintext, const std::string &sealed_hex) {
    const auto key = fixed<32>(key_hex);
    const auto nonce = fixed<12>(nonce_hex);
    const auto aad = hex(aad_hex);
    const auto sealed = hex(sealed_hex);
    std::vector<std::uint8_t> ciphertext(plaintext.size());
    const Tag16 tag = aead_seal(key, nonce, aad.data(), aad.size(), plaintext.data(), ciphertext.data(), plaintext.size());
    std::vector<std::uint8_t> combined = ciphertext;
    combined.insert(combined.end(), tag.begin(), tag.end());
    CHECK(combined == sealed);

    std::vector<std::uint8_t> opened(plaintext.size());
    CHECK(aead_open(key, nonce, aad.data(), aad.size(), ciphertext.data(), opened.data(), ciphertext.size(), tag));
    CHECK(opened == plaintext);

    // Any change to the ciphertext, the associated data or the tag is caught.
    Tag16 bad_tag = tag;
    bad_tag[0] ^= 1;
    CHECK(!aead_open(key, nonce, aad.data(), aad.size(), ciphertext.data(), opened.data(), ciphertext.size(), bad_tag));
    if (!ciphertext.empty()) {
      auto tampered = ciphertext;
      tampered.back() ^= 0x80;
      CHECK(!aead_open(key, nonce, aad.data(), aad.size(), tampered.data(), opened.data(), tampered.size(), tag));
    }
    auto other_aad = aad;
    other_aad.push_back(0);
    CHECK(!aead_open(key, nonce, other_aad.data(), other_aad.size(), ciphertext.data(), opened.data(), ciphertext.size(), tag));
  }

  void test_aead() {
    // RFC 8439 section 2.8.2.
    const std::string sunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    check_aead("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", "070000004041424344454647",
               "50515253c0c1c2c3c4c5c6c7", std::vector<std::uint8_t>(sunscreen.begin(), sunscreen.end()),
               "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691");

    // Lengths 0, 1, 15, 16, 17, 64, 65 and 130: every padding and block boundary.
    struct Vector {
      const char *key, *nonce, *aad, *plaintext, *sealed;
    };
    const Vector vectors[] = {
      {"a819408ce5010ca2e09ef59ac3d89f5ff8595d02b524e61bf8afa894a95d594f", "648f8e193a06c30767e71fb3", "", "", "8e66ca7c23f9af4914537784fada4d94"},
      {"8174099687a26621f4e2cdd7cc03b3dacedb3fb962255b1aafd033cabe831530", "0a78009591722cc84825ca95", "7f", "77", "b97a0176dee7a13b146a2102abc12a8da4"},
      {"6b4e7621b69f1b9e9a0579f021f78c34006ff840a27f7026e1b698a9d3eac1a1", "5e47fbf849d7ff1c10d93993", "133d7e47484ca315c42cc5857461d2", "0bbbf3de4af81bfba721137f250da9", "f35c0cfdc562a987d6beb025c1cdae8045f2b87bdab5853f9cf63e5f0d2bcd"},
      {"a94963d1046f43c9b6749c90fbf32ed9fa61cb21449039bf16fbcaa2c812508e", "6e601a9b134d7d304c3bd32a", "35a7b4e7a3c25ff41dc4c2a8880f3bd9", "8b91f51c8c12aedb537749978617f13d", "c85b190795c7d87e233e0c2f113b7bb071e0e6e24703dfcc65f83b1fcad3128a"},
      {"aa9289d9eb73a66807b3df01bdc5dd9cef06ee67798469aa03111fa679fd6fff", "a14f2c44eca3fc13f6801f82", "7bbfb3e5ca049f2e46fd214942f00adcc7", "e8f1a964fef34bdf27b7909366597be253", "b12748de9a099e5e80ba9c3278961f2b8b14bf16f81782b813f721089814730d92"},
      {"7d47d1603b326456dcf95b01cf950c294af62b96b2dbdf7ac009fa362911e657", "3c70d97860388d3afc19c6a3", "70", "437bda751dad48594f78cf7b535626c819ef64cdd8535c9cde472d0d0862a7c2437bda751dad48594f78cf7b535626c819ef64cdd8535c9cde472d0d0862a7c2", "36303373c3c535d1a5551bb1fedebd5b0a643ce8e01e0a9d6cd1fe8c0df0c2c1d259f4a75c3da78a1b0b8319a3ecb821e64c6b8bdba393608c677b56f7adf541d07b6597ecaae8789ef4ddeaca0954e1"},
      {"db0f0c08a28c2f3be70618477cdc9050f885e36630ffd9ff771f22eb24964b80", "31cf90def41cd82590326cc0", "0c67", "52a8be3238cc3d04bd743f1804efb3a28caaea00fb6e6aef7fdb643ab4a4246052a8be3238cc3d04bd743f1804efb3a28caaea00fb6e6aef7fdb643ab4a4246052", "095a4012cee018085991d4f20ed0456d10bf174908050b7c94d69a8f8f2ee7f6e193c1d65e9012ca7833fea131913f90fc237ff87d5c640a9773bc56d829a5f255edc0db91c6d54b5b3f1eddc558dbca76"},
      {"bd8100c840235664f22f2113593e880e4d78fce8e9688d6407bc62b60443efc8", "754b60ac68c8870381e44561", "d4ad1155", "10aa17dd6f404a7c27fb93f04a44448f199ce8e69d38da502cd90f9ae5059e3f10aa17dd6f404a7c27fb93f04a44448f199ce8e69d38da502cd90f9ae5059e3f10aa17dd6f404a7c27fb93f04a44448f199ce8e69d38da502cd90f9ae5059e3f10aa17dd6f404a7c27fb93f04a44448f199ce8e69d38da502cd90f9ae5059e3f10aa", "3572387f7f609d42aec0431c7c50be6a5d7215e389ac7428f22ce396c0c3740e0ad57201e22a154535f5fd78a829dbf4298d7e64ef75cec22f8bcb94cceaba46382abc37009611691ceab23b5d51005a50b6495d4b8ae6cfa35a2d69ff7dd50ca0b60cf2f6d4d6a82054c66ca7fdc1537a39b241e4bc181afb291ff61628289fd00c9b27cdfa987c7c04edd208537cfc0d48"},
    };
    for (const auto &v : vectors) {
      check_aead(v.key, v.nonce, v.aad, hex(v.plaintext), v.sealed);
    }
  }

}  // namespace

int main() {
  test_x25519();
  test_poly1305();
  test_aead();
  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}
