/**
 * @file link_protocol.h
 * @brief The UDP "link" between a streaming client and inputline-host.
 *
 * The client (the InputLine app on an iPad or iPhone) sends the controller's
 * raw HID reports straight to inputline-host on the PC, next to the
 * streaming app's own stream. Keeping the controller on its own channel means the streaming host
 * (Vibepollo, Sunshine, Apollo...) needs no changes at all.
 *
 * Every datagram starts with a 20-byte header. Pairing is CPace, a
 * password-authenticated key exchange keyed by the 6-digit code shown on the
 * PC: the pairing key never crosses the network, and even someone in the
 * middle of the exchange gets only one guess at the code per attempt. Session datagrams are encrypted and authenticated with
 * ChaCha20-Poly1305 and carry a strictly increasing counter, so nobody on the
 * network can read, inject or replay controller input. See docs/protocol.md
 * and SECURITY.md.
 *
 * Compatibility: the header carries the protocol version. Probe and
 * ProbeReply are understood by every version (their layouts only ever grow),
 * so an app and a PC of different versions can always tell the user which one
 * to update. Everything else requires both sides to speak kVersion.
 */
#pragma once

#include "crypto.h"
#include "triton.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inputline::link {

  constexpr std::uint16_t kDefaultPort = 48150;
  /** "CLK1" on the wire. Never changes: every version recognises every other by it. */
  constexpr std::uint32_t kMagic = 0x314B4C43;
  /** Protocol version this build speaks. */
  constexpr std::uint8_t kVersion = 3;
  /** Oldest protocol version this build still accepts. */
  constexpr std::uint8_t kMinVersion = 3;
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
    std::uint8_t version = kVersion;  ///< protocol version on the wire
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

  /**
   * @brief Parse the header without checking the tag.
   * @return nullopt for malformed input, or a version other than kVersion
   *         (except for Probe and ProbeReply, which every version reads).
   */
  std::optional<Header> peek(const std::uint8_t *data, std::size_t length);

  /** The protocol version of anything that looks like a link datagram (right magic), else nullopt. */
  std::optional<std::uint8_t> wire_version(const std::uint8_t *data, std::size_t length);

  /**
   * @brief Parse and authenticate (and for session types, decrypt) a datagram.
   * @param key Required unless the type is unauthenticated; ignored otherwise.
   * @return nullopt if malformed or the tag does not verify.
   */
  std::optional<Datagram> open(const std::uint8_t *data, std::size_t length, const Key *key);

  /** One key per direction, so counters never repeat a nonce. */
  struct SessionKeys {
    Key client_to_host {};
    Key host_to_client {};
  };

  /** Derived from the pairing key and both sides' fresh nonces, per session. */
  SessionKeys derive_session_keys(const Key &pairing_key, std::uint64_t client_nonce, std::uint64_t host_nonce);

  /**
   * Pairing is CPace (draft-irtf-cfrg-cpace, CPACE-X25519-SHA512): the client
   * is the initiator, the host the responder. Both derive a secret generator
   * from the code, the client's ID and the attempt's nonce, and exchange
   * public shares computed on it.
   */
  crypto::Key32 pairing_generator(const std::string &pin, std::uint32_t client_id, std::uint64_t nonce);

  /**
   * What both sides derive from a pairing exchange. The pairing key and both
   * confirmations depend on the code; a side that used another code derives
   * different ones.
   */
  struct PairingKeys {
    Key pairing_key {};
    Key result_key {};  ///< authenticates an accepting PairResult (the host's confirmation)
    std::array<std::uint8_t, kTagSize> proof {};  ///< the client's confirmation, in the PairRequest
  };

  /**
   * @param own_scalar This side's secret for this attempt (32 random bytes, never reused).
   * @param peer_share The other side's public share.
   * @return nullopt if the peer's share is invalid (a low-order point): abort the attempt.
   */
  std::optional<PairingKeys> derive_pairing_keys(
    const Key &own_scalar, const Key &peer_share, std::uint32_t client_id, const Key &client_share,
    const Key &host_share, std::uint64_t nonce, const std::string &client_name
  );

  /**
   * Authenticates a PairResult that says "wrong code". It is derived from
   * public values only, because the two sides don't share a key then; it
   * only tells the client which attempt the answer is for.
   */
  Key pairing_failure_key(std::uint32_t client_id, const Key &client_share, const Key &host_share, std::uint64_t nonce);

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

  /** Software version string length limit (e.g. "0.2.0-beta.1"). */
  constexpr std::size_t kMaxSoftwareVersionLength = 32;

  struct ProbeReply {
    std::uint64_t nonce = 0;
    bool pairing_open = false;
    std::string host_name;
    // From version 2 on (a version 1 reply reads as min = max = 1):
    std::uint8_t min_version = kMinVersion;
    std::uint8_t max_version = kVersion;
    std::string software_version;  ///< inputline-host's own version, for messages
    Key pairing_public_key {};  ///< in answer to a PairStart while pairing is open: the host's CPace share for it; else zero
  };

  /** Whether this build and the host that sent @p reply can talk, and if not, which side to update. */
  enum class Compatibility {
    kCompatible,
    kUpdateHost,  ///< the host only speaks older versions
    kUpdateClient,  ///< the host needs a newer client
  };

  Compatibility check_compatibility(const ProbeReply &reply, std::uint8_t client_version = kVersion);

  /**
   * A client that is not paired asks the host to show a pairing code. The host
   * picks the code, shows it on the PC's screen (which the user sees through
   * the stream), and answers with a ProbeReply saying whether pairing is open,
   * carrying its CPace share for this attempt (the header's client ID and the
   * nonce identify the attempt).
   */
  struct PairStart {
    std::uint64_t nonce = 0;
    std::string client_name;
  };

  struct PairRequest {
    Key client_public_key {};  ///< the client's CPace share
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
    std::string software_version;  ///< the app's version, for the host's log
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

  /**
   * Attach flag: the controller has stayed connected to the client, and to
   * the PC's settings, since its last AttachAck. A virtual controller the
   * host still has plugged in for it can carry on as it is. Without it, the
   * controller starts afresh and Steam has to set it up again.
   */
  constexpr std::uint8_t kAttachKeptSettings = 0x01;

  struct Attach {
    std::uint8_t controller = 0;
    DeviceKind kind = DeviceKind::kSteamController2026;
    Transport transport = Transport::kBluetoothLe;
    std::uint16_t vendor_id = kValveVendorId;
    std::uint16_t product_id = kTritonBleProductId;
    /** Real GET_ATTRIBUTES_VALUES reply read by the client, or all zero for host defaults. */
    std::array<std::uint8_t, kFeatureReportSize> attributes_reply {};
    std::array<char, kSerialBytes> unit_serial {};
    std::uint8_t flags = 0;  ///< kAttach* bits
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
  /** @param version The header's protocol version: the layout differs for version 1. */
  std::optional<ProbeReply> decode_probe_reply(const std::vector<std::uint8_t> &payload, std::uint8_t version = kVersion);
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

}  // namespace inputline::link
