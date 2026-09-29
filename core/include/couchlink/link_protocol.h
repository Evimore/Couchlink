/**
 * @file link_protocol.h
 * @brief The UDP "link" between a streaming client and couchlink-host.
 *
 * The client (the Couchlink app on an iPad or iPhone) sends the controller's
 * raw HID reports straight to couchlink-host on the gaming PC, next to the
 * streaming app's own stream. Keeping the controller on its own channel means the streaming host
 * (Vibepollo, Sunshine, Apollo...) needs no changes at all.
 *
 * Every datagram starts with a 20-byte header. After pairing, datagrams carry
 * a truncated HMAC-SHA-256 tag and a strictly increasing counter, so nobody
 * on the network can inject or replay controller input. The payload is not
 * encrypted: it is controller state. See docs/protocol.md and SECURITY.md.
 */
#pragma once

#include "triton.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace couchlink::link {

  constexpr std::uint16_t kDefaultPort = 48150;
  constexpr std::uint32_t kMagic = 0x314B4C43;  // "CLK1" on the wire
  constexpr std::uint8_t kVersion = 1;
  constexpr std::size_t kHeaderSize = 20;
  constexpr std::size_t kTagSize = 16;
  constexpr std::size_t kKeySize = 32;
  constexpr std::size_t kMaxPayload = 160;
  constexpr std::size_t kMaxDatagram = kHeaderSize + kMaxPayload + kTagSize;
  constexpr std::size_t kMaxNameLength = 32;
  constexpr std::size_t kSerialBytes = 20;
  constexpr std::size_t kPinDigits = 6;

  using Key = std::array<std::uint8_t, kKeySize>;

  enum class Type : std::uint8_t {
    // Unauthenticated: discovery and pairing.
    kProbe = 0x01,
    kProbeReply = 0x02,
    kPairRequest = 0x03,
    kPairStart = 0x04,  ///< "Show a pairing code on your screen"; answered with a ProbeReply.
    // Authenticated with the long-term pairing key.
    kHello = 0x10,
    kHelloAck = 0x11,
    kPairResult = 0x12,
    // Authenticated with the per-session key.
    kAttach = 0x20,
    kAttachAck = 0x21,
    kInput = 0x22,
    kInputBundle = 0x25,  ///< Input plus the previous report, to survive a lost datagram.
    kDetach = 0x23,
    kNeedAttach = 0x24,
    kPing = 0x30,
    kPong = 0x31,
    kHidOutput = 0x40,
    kBye = 0x50,
  };

  /** Which key authenticates a datagram type. */
  enum class KeyKind {
    kNone,
    kPairing,
    kSession,
  };

  KeyKind key_kind(Type type);

  struct Header {
    Type type = Type::kProbe;
    std::uint16_t payload_length = 0;
    std::uint32_t client_id = 0;
    std::uint64_t counter = 0;
  };

  struct Datagram {
    Header header;
    std::vector<std::uint8_t> payload;
  };

  /**
   * @brief Build a datagram.
   * @param key Required unless key_kind(header.type) is kNone.
   */
  std::vector<std::uint8_t> seal(const Header &header, const std::vector<std::uint8_t> &payload, const Key *key);

  /** Parse the header without checking the tag. Returns nullopt for malformed input. */
  std::optional<Header> peek(const std::uint8_t *data, std::size_t length);

  /**
   * @brief Parse and authenticate a datagram.
   * @param key Required unless the type is unauthenticated; ignored otherwise.
   * @return nullopt if malformed or the tag does not verify.
   */
  std::optional<Datagram> open(const std::uint8_t *data, std::size_t length, const Key *key);

  /** Session key = HMAC(pairing key, label || client nonce || host nonce). */
  Key derive_session_key(const Key &pairing_key, std::uint64_t client_nonce, std::uint64_t host_nonce);

  /** Pairing proof = HMAC(PIN, label || client id || key || nonce), truncated to kTagSize. */
  std::array<std::uint8_t, kTagSize> pairing_proof(const std::string &pin, std::uint32_t client_id, const Key &key, std::uint64_t nonce);

  /** Accepts only strictly increasing counters. */
  class ReplayGuard {
  public:
    bool accept(std::uint64_t counter);
    void reset();

  private:
    bool have_last_ = false;
    std::uint64_t last_ = 0;
  };

  // ---- Message bodies -------------------------------------------------------

  struct Probe {
    std::uint64_t nonce = 0;
  };

  struct ProbeReply {
    std::uint64_t nonce = 0;
    bool pairing_open = false;
    std::string host_name;
  };

  /**
   * A client that is not paired asks the host to show a pairing code. The host
   * picks the code, shows it on the PC's screen (which the user sees through
   * the stream), and answers with a ProbeReply saying whether pairing is open.
   */
  struct PairStart {
    std::uint64_t nonce = 0;
    std::string client_name;
  };

  struct PairRequest {
    Key key {};
    std::uint64_t nonce = 0;
    std::string client_name;
    std::array<std::uint8_t, kTagSize> proof {};
  };

  struct PairResult {
    bool accepted = false;
  };

  struct Hello {
    std::uint64_t client_nonce = 0;
    std::string client_name;
  };

  constexpr std::uint32_t kHostCapSteamController2026 = 0x00000001;

  struct HelloAck {
    std::uint64_t client_nonce = 0;
    std::uint64_t host_nonce = 0;
    std::uint32_t capabilities = 0;
  };

  enum class DeviceKind : std::uint8_t {
    kSteamController2026 = 1,
  };

  enum class Transport : std::uint8_t {
    kBluetoothLe = 1,
    kUsb = 2,
  };

  struct Attach {
    std::uint8_t controller = 0;
    DeviceKind kind = DeviceKind::kSteamController2026;
    Transport transport = Transport::kBluetoothLe;
    std::uint16_t vendor_id = kValveVendorId;
    std::uint16_t product_id = kTritonBleProductId;
    /** Real GET_ATTRIBUTES_VALUES reply read by the client, or all zero for host defaults. */
    std::array<std::uint8_t, kFeatureReportSize> attributes_reply {};
    std::array<char, kSerialBytes> unit_serial {};
  };

  enum class AttachStatus : std::uint8_t {
    kOk = 0,
    kUnsupportedDevice = 1,
    kBackendUnavailable = 2,
    kAttachFailed = 3,
  };

  struct AttachAck {
    std::uint8_t controller = 0;
    AttachStatus status = AttachStatus::kOk;
  };

  struct Input {
    std::uint8_t controller = 0;
    std::vector<std::uint8_t> report;  ///< Raw HID input report, report ID first (1..64 bytes).
  };

  /**
   * An input report together with the one before it. Reports are numbered per
   * controller; if the datagram carrying report N was lost, the one carrying
   * N+1 still delivers it, at the cost of one report interval.
   */
  struct InputBundle {
    std::uint8_t controller = 0;
    std::uint32_t sequence = 0;  ///< of `report`; `previous` is sequence - 1
    std::vector<std::uint8_t> report;
    std::vector<std::uint8_t> previous;  ///< empty for the first report
  };

  struct ControllerRef {
    std::uint8_t controller = 0;
  };

  struct Ping {
    std::uint64_t client_time_us = 0;
  };

  enum class OutputKind : std::uint8_t {
    kOutputReport = 1,  ///< Haptics and rumble (report IDs 0x80-0x89).
    kSetFeature = 2,  ///< Settings command Steam sent that the real controller must also see.
  };

  struct HidOutput {
    std::uint8_t controller = 0;
    OutputKind kind = OutputKind::kOutputReport;
    std::vector<std::uint8_t> report;  ///< Report ID first (1..64 bytes).
  };

  std::vector<std::uint8_t> encode(const Probe &message);
  std::vector<std::uint8_t> encode(const ProbeReply &message);
  std::vector<std::uint8_t> encode(const PairStart &message);
  std::vector<std::uint8_t> encode(const PairRequest &message);
  std::vector<std::uint8_t> encode(const PairResult &message);
  std::vector<std::uint8_t> encode(const Hello &message);
  std::vector<std::uint8_t> encode(const HelloAck &message);
  std::vector<std::uint8_t> encode(const Attach &message);
  std::vector<std::uint8_t> encode(const AttachAck &message);
  std::vector<std::uint8_t> encode(const Input &message);
  std::vector<std::uint8_t> encode(const InputBundle &message);
  std::vector<std::uint8_t> encode(const ControllerRef &message);
  std::vector<std::uint8_t> encode(const Ping &message);
  std::vector<std::uint8_t> encode(const HidOutput &message);

  std::optional<Probe> decode_probe(const std::vector<std::uint8_t> &payload);
  std::optional<ProbeReply> decode_probe_reply(const std::vector<std::uint8_t> &payload);
  std::optional<PairStart> decode_pair_start(const std::vector<std::uint8_t> &payload);
  std::optional<PairRequest> decode_pair_request(const std::vector<std::uint8_t> &payload);
  std::optional<PairResult> decode_pair_result(const std::vector<std::uint8_t> &payload);
  std::optional<Hello> decode_hello(const std::vector<std::uint8_t> &payload);
  std::optional<HelloAck> decode_hello_ack(const std::vector<std::uint8_t> &payload);
  std::optional<Attach> decode_attach(const std::vector<std::uint8_t> &payload);
  std::optional<AttachAck> decode_attach_ack(const std::vector<std::uint8_t> &payload);
  std::optional<Input> decode_input(const std::vector<std::uint8_t> &payload);
  std::optional<InputBundle> decode_input_bundle(const std::vector<std::uint8_t> &payload);
  std::optional<ControllerRef> decode_controller_ref(const std::vector<std::uint8_t> &payload);
  std::optional<Ping> decode_ping(const std::vector<std::uint8_t> &payload);
  std::optional<HidOutput> decode_hid_output(const std::vector<std::uint8_t> &payload);

  /** Hex helpers for storing keys in text config files. */
  std::string to_hex(const std::uint8_t *data, std::size_t length);
  std::optional<Key> key_from_hex(const std::string &hex);

}  // namespace couchlink::link
