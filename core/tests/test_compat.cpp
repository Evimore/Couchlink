// Compatibility between app and PC versions.
//
// 1. The wire format is frozen: every message, an encrypted datagram and the
//    key derivations have fixed bytes below. If one of these fails, a change
//    alters what goes over the network: an app and a PC of different builds
//    would stop understanding each other. Bump link::kVersion instead (and keep
//    reading Probe/ProbeReply as described in link_protocol.h), then update
//    the fixtures.
// 2. Version negotiation: every combination of older/newer app and PC ends in
//    "compatible" or a clear "update the PC" / "update the app".

#include "inputline/cpace.h"
#include "inputline/link_client.h"
#include "inputline/link_protocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace inputline::link;

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

  /** Compare encoded bytes with a frozen fixture, printing the new bytes on a mismatch. */
  bool frozen(const char *name, const std::vector<std::uint8_t> &bytes, const std::string &expected) {
    const auto actual = to_hex(bytes.data(), bytes.size());
    if (actual != expected) {
      std::fprintf(stderr, "wire format of '%s' changed:\n  was %s\n  now %s\n", name, expected.c_str(), actual.c_str());
      return false;
    }
    return true;
  }

  Key pattern_key(std::uint8_t start) {
    Key key {};
    for (int i = 0; i < 32; ++i) {
      key[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(start + i);
    }
    return key;
  }

  std::vector<std::uint8_t> bytes_of(const Key &key) {
    return {key.begin(), key.end()};
  }

  void test_frozen_wire_format() {
    CHECK(kVersion == 3);  // bumping it? update the fixtures below together
    CHECK(frozen("probe", encode(Probe {0x0102030405060708}), "0807060504030201"));

    ProbeReply reply;
    reply.nonce = 0x1122334455667788;
    reply.pairing_open = true;
    reply.host_name = "GAMING-PC";
    reply.min_version = 2;
    reply.max_version = 2;
    reply.software_version = "0.2.0";
    reply.pairing_public_key = pattern_key(0x40);
    CHECK(frozen("probe reply", encode(reply), "8877665544332211010947414d494e472d5043020205302e322e30404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"));

    CHECK(frozen("pair start", encode(PairStart {0x0A0B0C0D0E0F1011, "iPad"}), "11100f0e0d0c0b0a0469506164"));

    PairRequest request;
    request.client_public_key = pattern_key(0x60);
    request.nonce = 0x2122232425262728;
    request.client_name = "iPad";
    for (int i = 0; i < 16; ++i) {
      request.proof[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(0xA0 + i);
    }
    CHECK(frozen("pair request", encode(request), "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f28272625242322210469506164a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"));
    CHECK(frozen("pair result", encode(PairResult {true}), "01"));
    CHECK(frozen("hello", encode(Hello {0x3132333435363738, "iPad", "0.2.0"}), "3837363534333231046950616405302e322e30"));
    CHECK(frozen("hello ack", encode(HelloAck {0x4142434445464748, 0x5152535455565758, kHostCapSteamController2026}), "4847464544434241585756555453525101000000"));

    Attach attach;
    attach.controller = 1;
    attach.attributes_reply[0] = 1;
    attach.attributes_reply[1] = 0x83;
    std::memcpy(attach.unit_serial.data(), "FXA0123", 7);
    CHECK(frozen("attach", encode(attach), "01010100de280313018300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004658413031323300000000000000000000000000"));
    CHECK(frozen("attach ack", encode(AttachAck {1, AttachStatus::kOk}), "0100"));

    InputBundle bundle;
    bundle.controller = 1;
    bundle.sequence = 0x01020304;
    bundle.report = {0x45, 1, 2, 3};
    bundle.previous = {0x45, 0, 1, 2};
    CHECK(frozen("input bundle", encode(bundle), "010403020104450102030445000102"));
    CHECK(frozen("controller ref", encode(ControllerRef {2}), "02"));
    CHECK(frozen("ping", encode(Ping {0x6162636465666768}), "6867666564636261"));

    HidOutput output;
    output.controller = 1;
    output.kind = OutputKind::kSetFeature;
    output.report = {1, 0x87, 3, 9, 0, 0};
    CHECK(frozen("hid output", encode(output), "010206018703090000"));

    // A whole encrypted session datagram and an authenticated hello.
    const Key key = pattern_key(0x10);
    Header session;
    session.type = Type::kInputBundle;
    session.client_id = 0xCAFEBABE;
    session.counter = 42;
    CHECK(frozen("sealed session datagram", seal(session, encode(bundle), &key), "434c4b3103250f00bebafeca2a000000000000003c190b18bd252488ff77d678c6b5516b577204b485e1dafc903529c4c4411d"));
    Header hello;
    hello.type = Type::kHello;
    hello.client_id = 0xCAFEBABE;
    hello.counter = 1000;
    CHECK(frozen("sealed hello", seal(hello, encode(Hello {0x3132333435363738, "iPad", "0.2.0"}), &key), "434c4b3103101300bebafecae8030000000000003837363534333231046950616405302e322e30149e274a6143ee1e08149c89a0549b56"));

    // Key derivations.
    CHECK(frozen("session key", bytes_of(derive_session_keys(key, 1, 2).client_to_host), "77dce3f81df8ce51c60ee8247080754969d32d3c8e0ebe2eae77e73e78e87e52"));
    // Pairing (CPace; its own test vectors are in test_crypto.cpp).
    const auto generator = pairing_generator("123456", 7, 99);
    CHECK(frozen("pairing generator", bytes_of(generator), "a9fc4318937652e56782cadbeb912fbecf7c393795c85d7d003857ae68d0284d"));
    const auto client_share = inputline::cpace::public_share(pattern_key(0x20), generator);
    const auto host_share = inputline::cpace::public_share(pattern_key(0x30), generator);
    const auto keys = derive_pairing_keys(pattern_key(0x20), host_share, 7, client_share, host_share, 99, "iPad");
    CHECK(keys.has_value());
    if (keys) {
      CHECK(frozen("pairing key", bytes_of(keys->pairing_key), "a376b640d4c89eb69969a2c1c7960b3806f0d7362d8da0b1e6e8137b16611901"));
      CHECK(frozen("pairing proof", std::vector<std::uint8_t>(keys->proof.begin(), keys->proof.end()), "20b167b777f1448b6e6b2ed3a47021ee"));
      CHECK(frozen("pairing result key", bytes_of(keys->result_key), "5db23d5f074dff5b80c4f33b07cbb050291aac81c4ddac7d9987e95bbf6e7602"));
    }
    CHECK(frozen("pairing failure key", bytes_of(pairing_failure_key(7, client_share, host_share, 99)), "ae00ad6579e067c99010efadca6f6e08441891860b82d82da68c4ad00f479229"));
  }

  std::vector<std::uint8_t> probe_reply_datagram(std::uint8_t version, const std::vector<std::uint8_t> &payload) {
    Header header;
    header.version = version;
    header.type = Type::kProbeReply;
    return seal(header, payload, nullptr);
  }

  void test_negotiation() {
    ProbeReply same;
    CHECK(check_compatibility(same) == Compatibility::kCompatible);

    // A PC that only speaks older versions: update the PC.
    ProbeReply older_pc;
    older_pc.min_version = 1;
    older_pc.max_version = 1;
    CHECK(check_compatibility(older_pc) == Compatibility::kUpdateHost);

    // A PC that no longer speaks this version: update the app.
    ProbeReply newer_pc;
    newer_pc.min_version = kVersion + 1;
    newer_pc.max_version = kVersion + 2;
    CHECK(check_compatibility(newer_pc) == Compatibility::kUpdateClient);

    // A newer PC that still speaks this version too: fine.
    ProbeReply wider_pc;
    wider_pc.min_version = kVersion;
    wider_pc.max_version = kVersion + 1;
    CHECK(check_compatibility(wider_pc) == Compatibility::kCompatible);

    // A newer app talking to this PC: the PC's range says "update the PC".
    CHECK(check_compatibility(same, kVersion + 1) == Compatibility::kUpdateHost);
  }

  void test_replies_across_versions() {
    // What a version 1 PC (InputLine 0.1) sends: nonce, pairing open, name.
    const std::vector<std::uint8_t> v1_payload = {8, 7, 6, 5, 4, 3, 2, 1, 0, 3, 'O', 'l', 'd'};
    const auto v1 = probe_reply_datagram(1, v1_payload);
    const auto from_v1 = ClientSession::parse_probe_reply(v1.data(), v1.size(), 0x0102030405060708);
    CHECK(from_v1 && from_v1->host_name == "Old" && from_v1->max_version == 1);
    CHECK(from_v1 && check_compatibility(*from_v1) == Compatibility::kUpdateHost);

    // What a version 2 PC (InputLine 0.2 betas, before CPace pairing) sends:
    // this app says to update the PC; an app of that version, reading this
    // PC's reply, says to update the app.
    ProbeReply v2;
    v2.nonce = 6;
    v2.host_name = "Beta";
    v2.min_version = 2;
    v2.max_version = 2;
    v2.software_version = "0.2.0-beta.2";
    const auto v2_datagram = probe_reply_datagram(2, encode(v2));
    const auto from_v2 = ClientSession::parse_probe_reply(v2_datagram.data(), v2_datagram.size(), 6);
    CHECK(from_v2 && from_v2->software_version == "0.2.0-beta.2");
    CHECK(from_v2 && check_compatibility(*from_v2) == Compatibility::kUpdateHost);
    ProbeReply this_pc;
    CHECK(check_compatibility(this_pc, 2) == Compatibility::kUpdateClient);

    // A future PC that appends fields this build doesn't know yet.
    ProbeReply future;
    future.nonce = 5;
    future.host_name = "New";
    future.min_version = kVersion;
    future.max_version = kVersion + 1;
    future.software_version = "9.0.0";
    auto payload = encode(future);
    payload.insert(payload.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    const auto v3 = probe_reply_datagram(kVersion + 1, payload);
    const auto from_future = ClientSession::parse_probe_reply(v3.data(), v3.size(), 5);
    CHECK(from_future && from_future->host_name == "New" && from_future->software_version == "9.0.0");
    CHECK(from_future && check_compatibility(*from_future) == Compatibility::kCompatible);

    // Any other message from another version is not read at all.
    Header other;
    other.version = 1;
    other.type = Type::kPairResult;
    const Key key = pattern_key(1);
    const auto old_result = seal(other, encode(PairResult {true}), &key);
    CHECK(!peek(old_result.data(), old_result.size()).has_value());
  }

}  // namespace

int main() {
  test_frozen_wire_format();
  test_negotiation();
  test_replies_across_versions();
  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}
