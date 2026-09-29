// Core library tests; run with `ctest` or the inputline_core_tests binary.

#include "inputline/cpace.h"
#include "inputline/feature_responder.h"
#include "inputline/report_converter.h"
#include "inputline/link_protocol.h"
#include "inputline/sha256.h"
#include "inputline/timing_stats.h"
#include "inputline/version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
  int g_failures = 0;
  int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

  using namespace inputline;

  TritonControls sample_controls() {
    TritonControls c {};
    c.seq = 7;
    c.buttons = kButtonA | kButtonR4 | kLeftPadTouch | kLeftPadClick | kRightGripTouch;
    c.trigger_left = 32767;
    c.trigger_right = 128;
    c.left_stick_x = -1234;
    c.left_stick_y = 4321;
    c.right_stick_x = 32767;
    c.right_stick_y = -32768;
    return c;
  }

  TritonPads sample_pads() {
    return TritonPads {-16384, 16384, 16384, 100, -100, 32767};
  }

  std::vector<std::uint8_t> ble45_report(std::uint32_t timestamp_us) {
    TritonStateBle ble {};
    ble.controls = sample_controls();
    ble.pads = sample_pads();
    ble.imu.timestamp_us = timestamp_us;
    ble.imu.accel[0] = 1;
    ble.imu.accel[1] = 2;
    ble.imu.accel[2] = 3;
    ble.imu.gyro[0] = -4;
    ble.imu.gyro[1] = -5;
    ble.imu.gyro[2] = -6;
    std::vector<std::uint8_t> out(kBleStateReportSize);
    out[0] = kReportStateBle;
    std::memcpy(out.data() + 1, &ble, sizeof(ble));
    return out;
  }

  std::vector<std::uint8_t> ble47_report(std::uint16_t ticks) {
    TritonStateTimestamped ts {};
    ts.controls = sample_controls();
    ts.trackpad_timestamp = 0xBEEF;
    ts.pads = sample_pads();
    ts.imu.timestamp_32us = ticks;
    ts.imu.gyro[2] = 99;
    std::vector<std::uint8_t> out(kBleStateReportSize);
    out[0] = kReportStateTimestamped;
    std::memcpy(out.data() + 1, &ts, sizeof(ts));
    return out;
  }

  TritonStateUsb decode_wired(const StateReport &report) {
    TritonStateUsb state;
    std::memcpy(&state, report.data() + 1, sizeof(state));
    return state;
  }

  void test_convert_ble45() {
    StateReportConverter converter;
    StateReport out {};
    const auto in = ble45_report(123456);
    CHECK(converter.to_wired(in.data(), in.size(), out));
    CHECK(out[0] == kReportState);

    const auto state = decode_wired(out);
    CHECK(state.controls.seq == 7);
    CHECK(state.controls.buttons == sample_controls().buttons);
    CHECK(state.controls.right_stick_y == -32768);
    CHECK(state.pads.right_pressure == 32767);
    CHECK(state.imu.timestamp_us == 123456);
    CHECK(state.imu.accel[2] == 3);
    CHECK(state.imu.gyro[0] == -4);
    CHECK(state.imu.quat[0] == kIdentityQuatW);
    CHECK(state.imu.quat[1] == 0 && state.imu.quat[2] == 0 && state.imu.quat[3] == 0);
  }

  void test_convert_ble47_strips_trackpad_timestamp() {
    StateReportConverter converter;
    StateReport out {};
    const auto in = ble47_report(10);
    CHECK(converter.to_wired(in.data(), in.size(), out));

    const auto state = decode_wired(out);
    CHECK(state.pads.left_x == -16384);  // pads realigned after the dropped field
    CHECK(state.pads.right_y == -100);
    CHECK(state.imu.gyro[2] == 99);
    CHECK(state.imu.timestamp_us == 320);
  }

  void test_convert_ble47_unwraps_imu_clock() {
    StateReportConverter converter;
    StateReport out {};

    auto in = ble47_report(0xFFF0);
    CHECK(converter.to_wired(in.data(), in.size(), out));
    const auto first = decode_wired(out).imu.timestamp_us;

    in = ble47_report(0x0010);  // wrapped: 0x20 ticks later
    CHECK(converter.to_wired(in.data(), in.size(), out));
    CHECK(decode_wired(out).imu.timestamp_us == first + 0x20 * 32);

    converter.reset();
    in = ble47_report(5);
    CHECK(converter.to_wired(in.data(), in.size(), out));
    CHECK(decode_wired(out).imu.timestamp_us == 5 * 32);
  }

  void test_convert_passthrough_and_rejects() {
    StateReportConverter converter;
    StateReport out {};

    std::vector<std::uint8_t> wired(kStateReportSize, 0xAB);
    wired[0] = kReportState;
    CHECK(converter.to_wired(wired.data(), wired.size(), out));
    CHECK(std::memcmp(out.data(), wired.data(), kStateReportSize) == 0);

    CHECK(!converter.to_wired(wired.data(), kStateReportSize - 1, out));
    const auto ble = ble45_report(1);
    CHECK(!converter.to_wired(ble.data(), ble.size() - 1, out));
    const std::uint8_t battery[] = {kReportBattery, 1, 2, 3};
    CHECK(!converter.to_wired(battery, sizeof(battery), out));
    CHECK(!converter.to_wired(nullptr, 0, out));
  }

  std::vector<std::uint8_t> feature_request(std::uint8_t command, std::uint8_t param = 0) {
    std::vector<std::uint8_t> request(kFeatureReportSize, 0);
    request[0] = kReportFeatureChannel1;
    request[1] = command;
    request[2] = 1;
    request[3] = param;
    return request;
  }

  void test_feature_attributes() {
    FeatureResponder responder;
    const auto request = feature_request(kCmdGetAttributesValues);
    CHECK(responder.on_set_feature(request.data(), request.size()) == FeatureDisposition::kAnswerLocally);

    const auto reply = responder.on_get_feature(kReportFeatureChannel1);
    // Byte-exact against HIDMaestro's hardware-grounded reply.
    const std::uint8_t expected[] = {0x01, 0x83, 0x19, 0x01, 0x02, 0x13, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                                     0x0a, 0x2e, 0xf9, 0xd2, 0x68, 0x04, 0x57, 0xd0, 0x18, 0x6a, 0x09, 0x48, 0x00,
                                     0x00, 0x00};
    CHECK(std::memcmp(reply.data(), expected, sizeof(expected)) == 0);
    CHECK(reply[sizeof(expected)] == 0);
  }

  void test_adopt_firmware_attributes() {
    auto identity = default_identity();
    const auto before = identity.attributes_reply;

    // The physical controller's reply, as read over Bluetooth: a different
    // product ID and a newer firmware build, in another record order.
    std::array<std::uint8_t, kFeatureReportSize> physical {};
    const std::uint8_t records[] = {0x01, 0x83, 0x14, 0x04, 0x11, 0x22, 0x33, 0x6b, 0x01, 0x03, 0x13, 0x00, 0x00,
                                    0x0a, 0x00, 0x00, 0x00, 0x00, 0x09, 0x99, 0x00, 0x00, 0x00};
    std::memcpy(physical.data(), records, sizeof(records));
    CHECK(adopt_firmware_attributes(identity, physical));

    auto expected = before;
    expected[19] = 0x11;  // firmware build record (tag 0x04 at offset 18)
    expected[20] = 0x22;
    expected[21] = 0x33;
    expected[22] = 0x6b;
    CHECK(identity.attributes_reply == expected);  // product, bootloader (empty) and board revision kept

    std::array<std::uint8_t, kFeatureReportSize> not_attributes {};
    not_attributes[1] = 0xAE;
    CHECK(!adopt_firmware_attributes(identity, not_attributes));
    CHECK(!adopt_firmware_attributes(identity, {}));
  }

  void test_without_settings() {
    // 0x87 with settings 48 (IMU) and 49 (wireless packet version).
    const std::vector<std::uint8_t> report {0x01, 0x87, 0x06, 48, 0x18, 0x00, 49, 0x02, 0x00};
    const auto kept = without_settings(report, {kSettingWirelessPacketVersion});
    CHECK((kept == std::vector<std::uint8_t> {0x01, 0x87, 0x03, 48, 0x18, 0x00, 0x00, 0x00, 0x00}));
    CHECK(without_settings(report, {}) == report);
    CHECK(without_settings({0x01, 0x87, 0x03, 49, 0x02, 0x00}, {49}).empty());
    const std::vector<std::uint8_t> other {0x01, 0x8F, 0x00};
    CHECK(without_settings(other, {49}) == other);
  }

  void test_timing_stats() {
    TimingStats stats;
    CHECK(stats.summary().reports == 0 && stats.summary().rate_hz == 0);

    // 1000 reports 4 ms apart, one 120 ms hiccup, then a pause that is not counted.
    std::uint64_t t = 1'000'000;
    for (int i = 0; i < 1000; ++i) {
      stats.add(t);
      t += 4000;
    }
    t += 116'000;
    stats.add(t);
    stats.break_sequence();
    stats.add(t + 5'000'000);

    const auto s = stats.summary();
    CHECK(s.reports == 1002);
    CHECK(s.rate_hz > 230 && s.rate_hz < 250);  // 1000 gaps over 4.116 s
    CHECK(s.p50_ms == 4.0 && s.p99_ms == 4.0);
    CHECK(s.max_ms == 120.0);
    CHECK(s.over_20ms == 1 && s.over_50ms == 1 && s.over_100ms == 1);
    CHECK(s.to_string().find("max 120 ms") != std::string::npos);

    stats.reset();
    CHECK(stats.summary().reports == 0);
  }

  void test_feature_strings() {
    FeatureResponder responder;
    auto request = feature_request(kCmdGetStringAttribute, 3);
    responder.on_set_feature(request.data(), request.size());
    auto reply = responder.on_get_feature(kReportFeatureChannel1);
    CHECK(reply[1] == kCmdGetStringAttribute);
    CHECK(reply[2] == 0x14);
    CHECK(reply[3] == 3);
    CHECK(std::string(reinterpret_cast<const char *>(&reply[4])) == "7054257d2da7");

    request = feature_request(kCmdGetStringAttribute, 0);
    responder.on_set_feature(request.data(), request.size());
    reply = responder.on_get_feature(kReportFeatureChannel1);
    CHECK(std::string(reinterpret_cast<const char *>(&reply[4])) == "MXA9960200000");

    request = feature_request(kCmdGetStringAttribute, 7);
    responder.on_set_feature(request.data(), request.size());
    reply = responder.on_get_feature(kReportFeatureChannel1);
    CHECK(reply[3] == 0xFF);
    CHECK(reply[4] == 0);

    FeatureResponder second {default_identity(2)};
    request = feature_request(kCmdGetStringAttribute, 1);
    second.on_set_feature(request.data(), request.size());
    reply = second.on_get_feature(kReportFeatureChannel1);
    CHECK(std::string(reinterpret_cast<const char *>(&reply[4])) == "FXA9960200002");
  }

  void test_feature_settings_echo_and_forward() {
    FeatureResponder responder;
    auto request = feature_request(kCmdSetSettingsValues);
    request[2] = 3;
    request[3] = 9;  // setting number (lizard mode in SDL's list)
    request[4] = 0;
    request[5] = 0;
    CHECK(responder.on_set_feature(request.data(), request.size()) == FeatureDisposition::kForward);
    const auto reply = responder.on_get_feature(kReportFeatureChannel1);
    CHECK(std::memcmp(reply.data(), request.data(), kFeatureReportSize) == 0);
  }

  void test_feature_policy() {
    CHECK(FeatureResponder::classify(kCmdFactoryReset) == FeatureDisposition::kBlocked);
    CHECK(FeatureResponder::classify(0xA9) == FeatureDisposition::kBlocked);  // set serial
    CHECK(FeatureResponder::classify(0xAD) == FeatureDisposition::kBlocked);  // enable pairing
    CHECK(FeatureResponder::classify(0xB7) == FeatureDisposition::kBlocked);  // audio update start
    CHECK(FeatureResponder::classify(0x42) == FeatureDisposition::kBlocked);  // unknown
    CHECK(FeatureResponder::classify(kCmdTurnOffController) == FeatureDisposition::kForward);
    CHECK(FeatureResponder::classify(kCmdCalibrateJoystick) == FeatureDisposition::kForward);
    CHECK(FeatureResponder::classify(kCmdCalibrateAnalogTriggers) == FeatureDisposition::kForward);
    CHECK(FeatureResponder::classify(kCmdCalibrateTrackpads) == FeatureDisposition::kForward);
    CHECK(FeatureResponder::classify(0xA9) == FeatureDisposition::kBlocked);  // set serial number
    CHECK(FeatureResponder::classify(kCmdTriggerHapticPulse) == FeatureDisposition::kForward);
    CHECK(FeatureResponder::classify(kCmdClearDigitalMappings) == FeatureDisposition::kForward);
    CHECK(FeatureResponder::classify(0x85) == FeatureDisposition::kBlocked);  // keyboard/mouse emulation back on
    CHECK(FeatureResponder::classify(kCmdDongleGetWirelessState) == FeatureDisposition::kAnswerLocally);

    FeatureResponder responder;
    const std::uint8_t wrong_report[] = {0x05, kCmdGetAttributesValues};
    CHECK(responder.on_set_feature(wrong_report, sizeof(wrong_report)) == FeatureDisposition::kBlocked);
    const auto empty = responder.on_get_feature(kReportFeatureChannel2);
    CHECK(empty[0] == kReportFeatureChannel2 && empty[1] == 0);

    const auto wireless = feature_request(kCmdDongleGetWirelessState);
    responder.on_set_feature(wireless.data(), wireless.size());
    const auto reply = responder.on_get_feature(kReportFeatureChannel1);
    CHECK(reply[1] == 0xB4 && reply[2] == 1 && reply[3] == 1);
  }

  std::string hex(const std::uint8_t *data, std::size_t length) {
    return inputline::link::to_hex(data, length);
  }

  void test_sha256_vectors() {
    // FIPS 180-4 examples.
    auto digest = Sha256::hash("abc", 3);
    CHECK(hex(digest.data(), digest.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest = Sha256::hash("", 0);
    CHECK(hex(digest.data(), digest.size()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    digest = Sha256::hash(two_blocks.data(), two_blocks.size());
    CHECK(hex(digest.data(), digest.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Streaming in odd-sized pieces must match one-shot hashing.
    const std::string million(1000000, 'a');
    Sha256 sha;
    for (std::size_t i = 0; i < million.size(); i += 997) {
      sha.update(million.data() + i, std::min<std::size_t>(997, million.size() - i));
    }
    digest = sha.finish();
    CHECK(hex(digest.data(), digest.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }

  void test_hmac_vectors() {
    // RFC 4231 test cases 1, 2 and 6 (key longer than the block size).
    const std::vector<std::uint8_t> key1(20, 0x0b);
    auto mac = HmacSha256::mac(key1.data(), key1.size(), "Hi There", 8);
    CHECK(hex(mac.data(), mac.size()) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    const std::string data2 = "what do ya want for nothing?";
    mac = HmacSha256::mac("Jefe", 4, data2.data(), data2.size());
    CHECK(hex(mac.data(), mac.size()) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    const std::vector<std::uint8_t> key6(131, 0xaa);
    const std::string data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    mac = HmacSha256::mac(key6.data(), key6.size(), data6.data(), data6.size());
    CHECK(hex(mac.data(), mac.size()) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    const std::uint8_t a[] = {1, 2, 3};
    const std::uint8_t b[] = {1, 2, 4};
    CHECK(constant_time_equal(a, a, 3));
    CHECK(!constant_time_equal(a, b, 3));
  }

  inputline::link::Key test_key(std::uint8_t fill) {
    inputline::link::Key key {};
    key.fill(fill);
    return key;
  }

  void test_link_seal_open() {
    using namespace inputline::link;
    const auto key = test_key(0x11);

    Header header;
    header.type = Type::kInput;
    header.client_id = 0xCAFEBABE;
    header.counter = 42;
    Input input;
    input.controller = 1;
    input.report = ble45_report(7);
    const auto payload = encode(input);
    const auto datagram = seal(header, payload, &key);
    CHECK(datagram.size() == kHeaderSize + payload.size() + kTagSize);
    CHECK(datagram[0] == 'C' && datagram[1] == 'L' && datagram[2] == 'K' && datagram[3] == '1');
    // The payload is encrypted: the report bytes don't appear on the wire.
    CHECK(!std::equal(payload.begin(), payload.end(), datagram.begin() + kHeaderSize));
    // The header is authenticated too: a changed counter is rejected.
    auto moved = datagram;
    moved[12] ^= 0x01;
    CHECK(!open(moved.data(), moved.size(), &key).has_value());

    const auto opened = open(datagram.data(), datagram.size(), &key);
    CHECK(opened.has_value());
    CHECK(opened->header.client_id == 0xCAFEBABE);
    CHECK(opened->header.counter == 42);
    const auto decoded = decode_input(opened->payload);
    CHECK(decoded.has_value() && decoded->report == input.report && decoded->controller == 1);

    // Wrong key, flipped payload bit, flipped tag bit, truncation: all rejected.
    const auto other = test_key(0x22);
    CHECK(!open(datagram.data(), datagram.size(), &other).has_value());
    auto tampered = datagram;
    tampered[kHeaderSize + 5] ^= 0x01;
    CHECK(!open(tampered.data(), tampered.size(), &key).has_value());
    tampered = datagram;
    tampered.back() ^= 0x80;
    CHECK(!open(tampered.data(), tampered.size(), &key).has_value());
    CHECK(!open(datagram.data(), datagram.size() - 1, &key).has_value());
    CHECK(!open(datagram.data(), datagram.size(), nullptr).has_value());

    // A session-authenticated type cannot be sealed without a key.
    CHECK(seal(header, payload, nullptr).empty());
  }

  void test_link_unauthenticated_types() {
    using namespace inputline::link;
    Header header;
    header.type = Type::kProbe;
    const auto datagram = seal(header, encode(Probe {77}), nullptr);
    CHECK(datagram.size() == kHeaderSize + 8);
    const auto opened = open(datagram.data(), datagram.size(), nullptr);
    CHECK(opened.has_value());
    CHECK(decode_probe(opened->payload)->nonce == 77);

    auto bad_magic = datagram;
    bad_magic[0] = 'X';
    CHECK(!peek(bad_magic.data(), bad_magic.size()).has_value());
    // Probes from any version are readable, so the host can answer them...
    auto future_probe = datagram;
    future_probe[4] = 9;
    CHECK(peek(future_probe.data(), future_probe.size()).has_value());
    CHECK(wire_version(future_probe.data(), future_probe.size()) == 9);
    // ...but other types must match this version exactly.
    Header hello;
    hello.type = Type::kHello;
    const auto key = test_key(1);
    auto other_hello = seal(hello, encode(Hello {1, "iPad", "0.2.0"}), &key);
    CHECK(peek(other_hello.data(), other_hello.size()).has_value());
    other_hello[4] = 1;
    CHECK(!peek(other_hello.data(), other_hello.size()).has_value());
    CHECK(wire_version(other_hello.data(), other_hello.size()) == 1);
    auto bad_type = datagram;
    bad_type[5] = 0x7F;
    CHECK(!peek(bad_type.data(), bad_type.size()).has_value());
    auto lying_length = datagram;
    lying_length[6] = 9;
    CHECK(!peek(lying_length.data(), lying_length.size()).has_value());
  }

  void test_link_messages_roundtrip() {
    using namespace inputline::link;

    ProbeReply reply;
    reply.nonce = 5;
    reply.pairing_open = true;
    reply.host_name = std::string(40, 'x');
    reply.software_version = "0.2.0";
    reply.pairing_public_key = test_key(9);
    auto reply2 = decode_probe_reply(encode(reply));
    CHECK(reply2 && reply2->pairing_open && reply2->host_name.size() == kMaxNameLength);
    CHECK(reply2 && reply2->min_version == kMinVersion && reply2->max_version == kVersion);
    CHECK(reply2 && reply2->software_version == "0.2.0" && reply2->pairing_public_key == test_key(9));

    InputBundle bundle;
    bundle.controller = 1;
    bundle.sequence = 0xABCDEF01;
    bundle.report = std::vector<std::uint8_t>(46, 0x45);
    bundle.previous = std::vector<std::uint8_t>(46, 0x44);
    const auto bundle2 = decode_input_bundle(encode(bundle));
    CHECK(bundle2 && bundle2->sequence == 0xABCDEF01 && bundle2->report == bundle.report && bundle2->previous == bundle.previous);
    bundle.previous.clear();
    const auto first = decode_input_bundle(encode(bundle));
    CHECK(first && first->previous.empty());

    const auto start = decode_pair_start(encode(PairStart {77, "Couch iPad"}));
    CHECK(start && start->nonce == 77 && start->client_name == "Couch iPad");
    CHECK(!decode_pair_start({1, 2, 3}).has_value());
    CHECK(key_kind(Type::kPairStart) == KeyKind::kNone);

    PairRequest pair;
    pair.client_public_key = test_key(3);
    pair.nonce = 99;
    pair.client_name = "Living room iPad";
    pair.proof[0] = 0xAB;
    const auto pair2 = decode_pair_request(encode(pair));
    CHECK(pair2 && pair2->client_public_key == pair.client_public_key && pair2->client_name == pair.client_name && pair2->proof == pair.proof);

    CHECK(decode_pair_result(encode(PairResult {true}))->accepted);
    const auto hello = decode_hello(encode(Hello {1, "iPad", "0.2.0"}));
    CHECK(hello && hello->client_name == "iPad" && hello->software_version == "0.2.0");
    const auto ack = decode_hello_ack(encode(HelloAck {1, 2, kHostCapSteamController2026}));
    CHECK(ack && ack->host_nonce == 2 && ack->capabilities == kHostCapSteamController2026);

    Attach attach;
    attach.controller = 3;
    attach.attributes_reply[1] = 0x83;
    std::memcpy(attach.unit_serial.data(), "FXA1", 4);
    const auto attach2 = decode_attach(encode(attach));
    CHECK(attach2 && attach2->controller == 3 && attach2->attributes_reply[1] == 0x83 &&
          std::memcmp(attach2->unit_serial.data(), "FXA1", 4) == 0 && attach2->product_id == kTritonBleProductId &&
          attach2->flags == 0);
    attach.flags = kAttachKeptSettings;
    CHECK(encode(attach)[3] == kAttachKeptSettings);  // the byte older versions left zero
    CHECK(decode_attach(encode(attach))->flags == kAttachKeptSettings);
    auto bad_attach = encode(attach);
    bad_attach[1] = 9;  // unknown device kind
    CHECK(!decode_attach(bad_attach).has_value());

    CHECK(decode_attach_ack(encode(AttachAck {2, AttachStatus::kAttachFailed}))->status == AttachStatus::kAttachFailed);
    CHECK(decode_controller_ref(encode(ControllerRef {4}))->controller == 4);
    CHECK(decode_ping(encode(Ping {123456789}))->client_time_us == 123456789);

    HidOutput output;
    output.kind = OutputKind::kSetFeature;
    output.report = {1, 0x87, 3, 9, 0, 0};
    const auto output2 = decode_hid_output(encode(output));
    CHECK(output2 && output2->kind == OutputKind::kSetFeature && output2->report == output.report);

    // Malformed bodies.
    Input empty;
    CHECK(!decode_input(encode(empty)).has_value());
    Input oversized;
    oversized.report.assign(65, 1);
    CHECK(!decode_input(encode(oversized)).has_value());
    auto trailing = encode(Ping {1});
    trailing.push_back(0);
    CHECK(!decode_ping(trailing).has_value());
    CHECK(!decode_hello({}).has_value());
  }

  void test_link_keys_and_replay() {
    using namespace inputline::link;
    const auto key = test_key(0x42);
    const auto s1 = derive_session_keys(key, 1, 2);
    CHECK(s1.client_to_host == derive_session_keys(key, 1, 2).client_to_host);
    CHECK(s1.client_to_host != s1.host_to_client);  // one key per direction
    CHECK(s1.client_to_host != derive_session_keys(key, 1, 3).client_to_host);
    CHECK(s1.client_to_host != derive_session_keys(test_key(0x43), 1, 2).client_to_host);
    CHECK(s1.client_to_host != key);

    // Pairing (CPace): with the same code, ID and nonce both sides derive
    // the same keys; a different code or ID gives different shares and keys.
    const auto client_secret = test_key(0x11);
    const auto host_secret = test_key(0x22);
    auto shares = [&](const std::string &client_pin, const std::string &host_pin, std::uint32_t id) {
      const Key client_share = inputline::cpace::public_share(client_secret, pairing_generator(client_pin, id, 99));
      const Key host_share = inputline::cpace::public_share(host_secret, pairing_generator(host_pin, id, 99));
      return std::make_pair(client_share, host_share);
    };
    const auto [client_share, host_share] = shares("123456", "123456", 7);
    const auto on_client = derive_pairing_keys(client_secret, host_share, 7, client_share, host_share, 99, "iPad");
    const auto on_host = derive_pairing_keys(host_secret, client_share, 7, client_share, host_share, 99, "iPad");
    CHECK(on_client && on_host);
    CHECK(on_client->pairing_key == on_host->pairing_key && on_client->proof == on_host->proof && on_client->result_key == on_host->result_key);
    const auto [wrong_client, right_host] = shares("123457", "123456", 7);
    const auto guess_client = derive_pairing_keys(client_secret, right_host, 7, wrong_client, right_host, 99, "iPad");
    const auto guess_host = derive_pairing_keys(host_secret, wrong_client, 7, wrong_client, right_host, 99, "iPad");
    CHECK(guess_client && guess_host && guess_client->proof != guess_host->proof && guess_client->pairing_key != guess_host->pairing_key);
    CHECK(guess_client->result_key != guess_host->result_key);
    // The client's name is authenticated too.
    const auto renamed = derive_pairing_keys(host_secret, client_share, 7, client_share, host_share, 99, "Not iPad");
    CHECK(renamed && renamed->proof != on_client->proof);
    // Nothing on the wire equals the code's generator or the pairing key.
    CHECK(client_share != pairing_generator("123456", 7, 99) && client_share != on_client->pairing_key);
    CHECK(!derive_pairing_keys(host_secret, Key {}, 7, Key {}, host_share, 99, "iPad").has_value());
    CHECK(pairing_failure_key(7, client_share, host_share, 99) != on_client->result_key);

    const auto hex_key = to_hex(key.data(), key.size());
    CHECK(key_from_hex(hex_key) == key);
    CHECK(!key_from_hex(hex_key.substr(1)).has_value());
    CHECK(!key_from_hex(std::string(64, 'g')).has_value());

    ReplayGuard guard;
    CHECK(guard.accept(0));
    CHECK(!guard.accept(0));
    CHECK(guard.accept(5));
    CHECK(!guard.accept(4));
    CHECK(guard.accept(6));
    guard.reset();
    CHECK(guard.accept(1));
  }
  void test_versions() {
    CHECK(compare_versions("0.2.0", "0.2.0") == 0);
    CHECK(compare_versions("v0.2.0", "0.2.0") == 0);
    CHECK(compare_versions("0.2.0", "0.10.0") < 0);
    CHECK(compare_versions("1.0.0", "0.99.99") > 0);
    CHECK(compare_versions("0.2", "0.2.0") == 0);
    CHECK(compare_versions("0.2.0-beta.2", "0.2.0") < 0);
    CHECK(compare_versions("0.2.0", "0.2.0-beta.2") > 0);
    CHECK(compare_versions("0.2.0-beta.2", "0.2.0-beta.10") < 0);
    CHECK(compare_versions("0.2.0-beta.1", "0.2.0-beta") > 0);
    CHECK(compare_versions("0.2.0-alpha", "0.2.0-beta") < 0);
    CHECK(compare_versions("0.2.0-1", "0.2.0-alpha") < 0);
    CHECK(compare_versions("0.2.0+abc", "0.2.0+def") == 0);
    CHECK(compare_versions("0.3.0-beta.1", "0.2.9") > 0);
    CHECK(is_version("v0.2.0-beta.1") && is_version("1") && !is_version("") && !is_version("abc") && !is_version("1..2") && !is_version("1.2-"));
    CHECK(is_prerelease("0.2.0-beta.1") && !is_prerelease("0.2.0") && !is_prerelease("x-1"));
    CHECK(compare_versions("99999999999999999999999", "1") > 0);  // saturates, no overflow
  }

}  // namespace

int main() {
  test_convert_ble45();
  test_convert_ble47_strips_trackpad_timestamp();
  test_convert_ble47_unwraps_imu_clock();
  test_convert_passthrough_and_rejects();
  test_feature_attributes();
  test_feature_strings();
  test_feature_settings_echo_and_forward();
  test_feature_policy();
  test_adopt_firmware_attributes();
  test_timing_stats();
  test_without_settings();
  test_sha256_vectors();
  test_hmac_vectors();
  test_link_seal_open();
  test_link_unauthenticated_types();
  test_link_messages_roundtrip();
  test_link_keys_and_replay();
  test_versions();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}
