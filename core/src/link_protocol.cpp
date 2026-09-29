#include "inputline/link_protocol.h"

#include "inputline/sha256.h"

#include <algorithm>
#include <cstring>

namespace inputline::link {

  namespace {
    constexpr char kSessionLabel[] = "inputline session v2";
    constexpr char kClientToHostLabel[] = "client to host";
    constexpr char kHostToClientLabel[] = "host to client";
    constexpr char kPairLabel[] = "inputline pair v2";
    constexpr char kPairKeyLabel[] = "pairing key";
    constexpr char kPairResultLabel[] = "pairing result";
    constexpr char kPairProofLabel[] = "pairing proof";

    class Writer {
    public:
      void u8(std::uint8_t v) {
        out_.push_back(v);
      }

      void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
      }

      void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
      }

      void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
      }

      void bytes(const void *data, std::size_t length) {
        const auto *p = static_cast<const std::uint8_t *>(data);
        out_.insert(out_.end(), p, p + length);
      }

      /** Length-prefixed string, truncated to kMaxNameLength. */
      void name(const std::string &value) {
        const auto length = std::min(value.size(), kMaxNameLength);
        u8(static_cast<std::uint8_t>(length));
        bytes(value.data(), length);
      }

      std::vector<std::uint8_t> take() {
        return std::move(out_);
      }

    private:
      std::vector<std::uint8_t> out_;
    };

    class Reader {
    public:
      Reader(const std::uint8_t *data, std::size_t length):
          data_(data),
          length_(length) {}

      bool u8(std::uint8_t &v) {
        if (!have(1)) {
          return false;
        }
        v = data_[pos_++];
        return true;
      }

      bool u16(std::uint16_t &v) {
        if (!have(2)) {
          return false;
        }
        v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return true;
      }

      bool u32(std::uint32_t &v) {
        std::uint16_t lo = 0, hi = 0;
        if (!u16(lo) || !u16(hi)) {
          return false;
        }
        v = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
        return true;
      }

      bool u64(std::uint64_t &v) {
        std::uint32_t lo = 0, hi = 0;
        if (!u32(lo) || !u32(hi)) {
          return false;
        }
        v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
      }

      bool bytes(void *out, std::size_t length) {
        if (!have(length)) {
          return false;
        }
        std::memcpy(out, data_ + pos_, length);
        pos_ += length;
        return true;
      }

      bool name(std::string &value) {
        std::uint8_t length = 0;
        if (!u8(length) || length > kMaxNameLength || !have(length)) {
          return false;
        }
        value.assign(reinterpret_cast<const char *>(data_ + pos_), length);
        pos_ += length;
        return true;
      }

      /** Report prefixed by its length byte; 1..64 bytes. */
      bool report(std::vector<std::uint8_t> &out, std::uint8_t length) {
        if (length < 1 || length > kMaxReportSize || !have(length)) {
          return false;
        }
        out.assign(data_ + pos_, data_ + pos_ + length);
        pos_ += length;
        return true;
      }

      bool done() const {
        return pos_ == length_;
      }

      /** A string of up to @p max bytes, length-prefixed. */
      bool text(std::string &value, std::size_t max) {
        std::uint8_t length = 0;
        if (!u8(length) || length > max || !have(length)) {
          return false;
        }
        value.assign(reinterpret_cast<const char *>(data_ + pos_), length);
        pos_ += length;
        return true;
      }

    private:
      bool have(std::size_t n) const {
        return length_ - pos_ >= n;
      }

      const std::uint8_t *data_;
      std::size_t length_;
      std::size_t pos_ = 0;
    };

    std::array<std::uint8_t, kTagSize> tag_for(const Key &key, const std::uint8_t *data, std::size_t length) {
      const auto digest = HmacSha256::mac(key.data(), key.size(), data, length);
      std::array<std::uint8_t, kTagSize> tag {};
      std::memcpy(tag.data(), digest.data(), tag.size());
      return tag;
    }

    bool known_type(std::uint8_t type) {
      switch (static_cast<Type>(type)) {
        case Type::kProbe:
        case Type::kProbeReply:
        case Type::kPairRequest:
        case Type::kPairStart:
        case Type::kHello:
        case Type::kHelloAck:
        case Type::kPairResult:
        case Type::kAttach:
        case Type::kAttachAck:
        case Type::kInput:
        case Type::kInputBundle:
        case Type::kDetach:
        case Type::kNeedAttach:
        case Type::kPing:
        case Type::kPong:
        case Type::kHidOutput:
        case Type::kBye:
          return true;
      }
      return false;
    }

    Key hmac_key(const void *key, std::size_t key_length, const std::vector<std::uint8_t> &message) {
      const auto digest = HmacSha256::mac(key, key_length, message.data(), message.size());
      Key out {};
      std::memcpy(out.data(), digest.data(), out.size());
      return out;
    }

    /** Session nonce: four zero bytes, then the datagram counter. */
    crypto::Nonce12 session_nonce(std::uint64_t counter) {
      crypto::Nonce12 nonce {};
      for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<std::uint8_t>(counter >> (8 * i));
      }
      return nonce;
    }

    bool version_neutral(Type type) {
      return type == Type::kProbe || type == Type::kProbeReply;
    }

    template<typename T>
    std::optional<T> finish(Reader &reader, T value) {
      if (!reader.done()) {
        return std::nullopt;
      }
      return value;
    }
  }  // namespace

  KeyKind key_kind(Type type) {
    switch (type) {
      case Type::kProbe:
      case Type::kProbeReply:
      case Type::kPairRequest:
      case Type::kPairStart:
        return KeyKind::kNone;
      case Type::kHello:
      case Type::kHelloAck:
      case Type::kPairResult:
        return KeyKind::kPairing;
      default:
        return KeyKind::kSession;
    }
  }

  std::vector<std::uint8_t> seal(const Header &header, const std::vector<std::uint8_t> &payload, const Key *key) {
    Writer writer;
    writer.u32(kMagic);
    writer.u8(header.version);
    writer.u8(static_cast<std::uint8_t>(header.type));
    writer.u16(static_cast<std::uint16_t>(payload.size()));
    writer.u32(header.client_id);
    writer.u64(header.counter);
    writer.bytes(payload.data(), payload.size());
    auto out = writer.take();

    switch (key_kind(header.type)) {
      case KeyKind::kNone:
        break;
      case KeyKind::kPairing: {
        if (key == nullptr) {
          return {};
        }
        const auto tag = tag_for(*key, out.data(), out.size());
        out.insert(out.end(), tag.begin(), tag.end());
        break;
      }
      case KeyKind::kSession: {
        if (key == nullptr) {
          return {};
        }
        // The header stays readable (the receiver needs it) but is authenticated.
        const auto tag = crypto::aead_seal(
          *key, session_nonce(header.counter), out.data(), kHeaderSize, out.data() + kHeaderSize, out.data() + kHeaderSize, payload.size()
        );
        out.insert(out.end(), tag.begin(), tag.end());
        break;
      }
    }
    return out;
  }

  std::optional<Header> peek(const std::uint8_t *data, std::size_t length) {
    if (data == nullptr || length < kHeaderSize || length > kMaxDatagram) {
      return std::nullopt;
    }

    Reader reader(data, length);
    std::uint32_t magic = 0;
    std::uint8_t type = 0;
    Header header;
    reader.u32(magic);
    reader.u8(header.version);
    reader.u8(type);
    reader.u16(header.payload_length);
    reader.u32(header.client_id);
    reader.u64(header.counter);
    if (magic != kMagic || header.version == 0 || !known_type(type) || header.payload_length > kMaxPayload) {
      return std::nullopt;
    }
    header.type = static_cast<Type>(type);
    if (header.version != kVersion && !version_neutral(header.type)) {
      return std::nullopt;
    }

    const std::size_t expected = kHeaderSize + header.payload_length +
                                 (key_kind(header.type) == KeyKind::kNone ? 0 : kTagSize);
    if (length != expected) {
      return std::nullopt;
    }
    return header;
  }

  std::optional<Datagram> open(const std::uint8_t *data, std::size_t length, const Key *key) {
    const auto header = peek(data, length);
    if (!header) {
      return std::nullopt;
    }

    Datagram datagram;
    datagram.header = *header;
    datagram.payload.assign(data + kHeaderSize, data + kHeaderSize + header->payload_length);

    switch (key_kind(header->type)) {
      case KeyKind::kNone:
        break;
      case KeyKind::kPairing: {
        if (key == nullptr) {
          return std::nullopt;
        }
        const std::size_t signed_length = length - kTagSize;
        const auto expected = tag_for(*key, data, signed_length);
        if (!constant_time_equal(expected.data(), data + signed_length, kTagSize)) {
          return std::nullopt;
        }
        break;
      }
      case KeyKind::kSession: {
        if (key == nullptr) {
          return std::nullopt;
        }
        crypto::Tag16 tag {};
        std::memcpy(tag.data(), data + length - kTagSize, kTagSize);
        if (!crypto::aead_open(
              *key, session_nonce(header->counter), data, kHeaderSize, datagram.payload.data(), datagram.payload.data(),
              datagram.payload.size(), tag
            )) {
          return std::nullopt;
        }
        break;
      }
    }
    return datagram;
  }

  std::optional<std::uint8_t> wire_version(const std::uint8_t *data, std::size_t length) {
    if (data == nullptr || length < kHeaderSize) {
      return std::nullopt;
    }
    Reader reader(data, length);
    std::uint32_t magic = 0;
    std::uint8_t version = 0;
    reader.u32(magic);
    reader.u8(version);
    if (magic != kMagic) {
      return std::nullopt;
    }
    return version;
  }

  SessionKeys derive_session_keys(const Key &pairing_key, std::uint64_t client_nonce, std::uint64_t host_nonce) {
    Writer writer;
    writer.bytes(kSessionLabel, sizeof(kSessionLabel) - 1);
    writer.u64(client_nonce);
    writer.u64(host_nonce);
    const Key base = hmac_key(pairing_key.data(), pairing_key.size(), writer.take());

    SessionKeys keys;
    keys.client_to_host = hmac_key(base.data(), base.size(), std::vector<std::uint8_t>(kClientToHostLabel, kClientToHostLabel + sizeof(kClientToHostLabel) - 1));
    keys.host_to_client = hmac_key(base.data(), base.size(), std::vector<std::uint8_t>(kHostToClientLabel, kHostToClientLabel + sizeof(kHostToClientLabel) - 1));
    return keys;
  }

  std::optional<PairingKeys> derive_pairing_keys(
    const Key &own_secret, const Key &peer_public, std::uint32_t client_id, const Key &client_public,
    const Key &host_public, std::uint64_t nonce, const std::string &pin
  ) {
    Key shared {};
    if (!crypto::x25519(own_secret, peer_public, shared)) {
      return std::nullopt;
    }
    // Everything both sides saw: binds the keys to this exchange.
    Writer transcript;
    transcript.bytes(kPairLabel, sizeof(kPairLabel) - 1);
    transcript.u32(client_id);
    transcript.bytes(client_public.data(), client_public.size());
    transcript.bytes(host_public.data(), host_public.size());
    transcript.u64(nonce);
    const auto base = transcript.take();

    auto with = [&base](const char *label, std::size_t label_length, const std::string &extra) {
      std::vector<std::uint8_t> message = base;
      message.insert(message.end(), label, label + label_length);
      message.insert(message.end(), extra.begin(), extra.end());
      return message;
    };

    PairingKeys keys;
    keys.pairing_key = hmac_key(shared.data(), shared.size(), with(kPairKeyLabel, sizeof(kPairKeyLabel) - 1, pin));
    keys.result_key = hmac_key(shared.data(), shared.size(), with(kPairResultLabel, sizeof(kPairResultLabel) - 1, {}));
    const Key proof = hmac_key(keys.pairing_key.data(), keys.pairing_key.size(), with(kPairProofLabel, sizeof(kPairProofLabel) - 1, {}));
    std::memcpy(keys.proof.data(), proof.data(), keys.proof.size());
    shared.fill(0);
    return keys;
  }

  Compatibility check_compatibility(const ProbeReply &reply, std::uint8_t client_version) {
    if (client_version > reply.max_version) {
      return Compatibility::kUpdateHost;
    }
    if (client_version < reply.min_version) {
      return Compatibility::kUpdateClient;
    }
    return Compatibility::kCompatible;
  }

  bool ReplayGuard::accept(std::uint64_t counter) {
    if (have_last_ && counter <= last_) {
      return false;
    }
    have_last_ = true;
    last_ = counter;
    return true;
  }

  void ReplayGuard::reset() {
    have_last_ = false;
    last_ = 0;
  }

  // ---- Encoders ---------------------------------------------------------------

  std::vector<std::uint8_t> encode(const Probe &message) {
    Writer w;
    w.u64(message.nonce);
    return w.take();
  }

  std::vector<std::uint8_t> encode(const ProbeReply &message) {
    // Only ever append fields: every version reads this message.
    Writer w;
    w.u64(message.nonce);
    w.u8(message.pairing_open ? 1 : 0);
    w.name(message.host_name);
    w.u8(message.min_version);
    w.u8(message.max_version);
    const auto version = message.software_version.substr(0, kMaxSoftwareVersionLength);
    w.u8(static_cast<std::uint8_t>(version.size()));
    w.bytes(version.data(), version.size());
    w.bytes(message.pairing_public_key.data(), message.pairing_public_key.size());
    return w.take();
  }

  std::vector<std::uint8_t> encode(const PairStart &message) {
    Writer w;
    w.u64(message.nonce);
    w.name(message.client_name);
    return w.take();
  }

  std::vector<std::uint8_t> encode(const PairRequest &message) {
    Writer w;
    w.bytes(message.client_public_key.data(), message.client_public_key.size());
    w.u64(message.nonce);
    w.name(message.client_name);
    w.bytes(message.proof.data(), message.proof.size());
    return w.take();
  }

  std::vector<std::uint8_t> encode(const PairResult &message) {
    Writer w;
    w.u8(message.accepted ? 1 : 0);
    return w.take();
  }

  std::vector<std::uint8_t> encode(const Hello &message) {
    Writer w;
    w.u64(message.client_nonce);
    w.name(message.client_name);
    const auto version = message.software_version.substr(0, kMaxSoftwareVersionLength);
    w.u8(static_cast<std::uint8_t>(version.size()));
    w.bytes(version.data(), version.size());
    return w.take();
  }

  std::vector<std::uint8_t> encode(const HelloAck &message) {
    Writer w;
    w.u64(message.client_nonce);
    w.u64(message.host_nonce);
    w.u32(message.capabilities);
    return w.take();
  }

  std::vector<std::uint8_t> encode(const Attach &message) {
    Writer w;
    w.u8(message.controller);
    w.u8(static_cast<std::uint8_t>(message.kind));
    w.u8(static_cast<std::uint8_t>(message.transport));
    w.u8(0);
    w.u16(message.vendor_id);
    w.u16(message.product_id);
    w.bytes(message.attributes_reply.data(), message.attributes_reply.size());
    w.bytes(message.unit_serial.data(), message.unit_serial.size());
    return w.take();
  }

  std::vector<std::uint8_t> encode(const AttachAck &message) {
    Writer w;
    w.u8(message.controller);
    w.u8(static_cast<std::uint8_t>(message.status));
    return w.take();
  }

  std::vector<std::uint8_t> encode(const Input &message) {
    Writer w;
    w.u8(message.controller);
    w.u8(static_cast<std::uint8_t>(message.report.size()));
    w.bytes(message.report.data(), message.report.size());
    return w.take();
  }

  std::vector<std::uint8_t> encode(const InputBundle &message) {
    Writer w;
    w.u8(message.controller);
    w.u32(message.sequence);
    w.u8(static_cast<std::uint8_t>(message.report.size()));
    w.bytes(message.report.data(), message.report.size());
    w.u8(static_cast<std::uint8_t>(message.previous.size()));
    w.bytes(message.previous.data(), message.previous.size());
    return w.take();
  }

  std::vector<std::uint8_t> encode(const ControllerRef &message) {
    Writer w;
    w.u8(message.controller);
    return w.take();
  }

  std::vector<std::uint8_t> encode(const Ping &message) {
    Writer w;
    w.u64(message.client_time_us);
    return w.take();
  }

  std::vector<std::uint8_t> encode(const HidOutput &message) {
    Writer w;
    w.u8(message.controller);
    w.u8(static_cast<std::uint8_t>(message.kind));
    w.u8(static_cast<std::uint8_t>(message.report.size()));
    w.bytes(message.report.data(), message.report.size());
    return w.take();
  }

  // ---- Decoders ---------------------------------------------------------------

  std::optional<Probe> decode_probe(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    Probe m;
    if (!r.u64(m.nonce)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<ProbeReply> decode_probe_reply(const std::vector<std::uint8_t> &payload, std::uint8_t version) {
    Reader r(payload.data(), payload.size());
    ProbeReply m;
    std::uint8_t open = 0;
    if (!r.u64(m.nonce) || !r.u8(open) || open > 1 || !r.name(m.host_name)) {
      return std::nullopt;
    }
    m.pairing_open = open == 1;
    if (version == 1) {
      m.min_version = m.max_version = 1;
      m.software_version.clear();
      return finish(r, m);
    }
    if (!r.u8(m.min_version) || !r.u8(m.max_version) || m.min_version > m.max_version ||
        !r.text(m.software_version, kMaxSoftwareVersionLength) ||
        !r.bytes(m.pairing_public_key.data(), m.pairing_public_key.size())) {
      return std::nullopt;
    }
    return m;  // later versions may append fields: ignore them
  }

  std::optional<PairStart> decode_pair_start(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    PairStart m;
    if (!r.u64(m.nonce) || !r.name(m.client_name)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<PairRequest> decode_pair_request(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    PairRequest m;
    if (!r.bytes(m.client_public_key.data(), m.client_public_key.size()) || !r.u64(m.nonce) || !r.name(m.client_name) ||
        !r.bytes(m.proof.data(), m.proof.size())) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<PairResult> decode_pair_result(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    std::uint8_t accepted = 0;
    if (!r.u8(accepted) || accepted > 1) {
      return std::nullopt;
    }
    return finish(r, PairResult {accepted == 1});
  }

  std::optional<Hello> decode_hello(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    Hello m;
    if (!r.u64(m.client_nonce) || !r.name(m.client_name) || !r.text(m.software_version, kMaxSoftwareVersionLength)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<HelloAck> decode_hello_ack(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    HelloAck m;
    if (!r.u64(m.client_nonce) || !r.u64(m.host_nonce) || !r.u32(m.capabilities)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<Attach> decode_attach(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    Attach m;
    std::uint8_t kind = 0, transport = 0, reserved = 0;
    if (!r.u8(m.controller) || !r.u8(kind) || !r.u8(transport) || !r.u8(reserved) || !r.u16(m.vendor_id) ||
        !r.u16(m.product_id) || !r.bytes(m.attributes_reply.data(), m.attributes_reply.size()) ||
        !r.bytes(m.unit_serial.data(), m.unit_serial.size())) {
      return std::nullopt;
    }
    if (kind != static_cast<std::uint8_t>(DeviceKind::kSteamController2026) ||
        (transport != static_cast<std::uint8_t>(Transport::kBluetoothLe) &&
         transport != static_cast<std::uint8_t>(Transport::kUsb))) {
      return std::nullopt;
    }
    m.kind = static_cast<DeviceKind>(kind);
    m.transport = static_cast<Transport>(transport);
    return finish(r, m);
  }

  std::optional<AttachAck> decode_attach_ack(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    AttachAck m;
    std::uint8_t status = 0;
    if (!r.u8(m.controller) || !r.u8(status) || status > static_cast<std::uint8_t>(AttachStatus::kAttachFailed)) {
      return std::nullopt;
    }
    m.status = static_cast<AttachStatus>(status);
    return finish(r, m);
  }

  std::optional<Input> decode_input(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    Input m;
    std::uint8_t length = 0;
    if (!r.u8(m.controller) || !r.u8(length) || !r.report(m.report, length)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<InputBundle> decode_input_bundle(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    InputBundle m;
    std::uint8_t length = 0;
    std::uint8_t previous_length = 0;
    if (!r.u8(m.controller) || !r.u32(m.sequence) || !r.u8(length) || !r.report(m.report, length) || !r.u8(previous_length)) {
      return std::nullopt;
    }
    if (previous_length > 0 && !r.report(m.previous, previous_length)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<ControllerRef> decode_controller_ref(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    ControllerRef m;
    if (!r.u8(m.controller)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<Ping> decode_ping(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    Ping m;
    if (!r.u64(m.client_time_us)) {
      return std::nullopt;
    }
    return finish(r, m);
  }

  std::optional<HidOutput> decode_hid_output(const std::vector<std::uint8_t> &payload) {
    Reader r(payload.data(), payload.size());
    HidOutput m;
    std::uint8_t kind = 0, length = 0;
    if (!r.u8(m.controller) || !r.u8(kind) || !r.u8(length) || !r.report(m.report, length)) {
      return std::nullopt;
    }
    if (kind != static_cast<std::uint8_t>(OutputKind::kOutputReport) &&
        kind != static_cast<std::uint8_t>(OutputKind::kSetFeature)) {
      return std::nullopt;
    }
    m.kind = static_cast<OutputKind>(kind);
    return finish(r, m);
  }

  std::string to_hex(const std::uint8_t *data, std::size_t length) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
      out.push_back(kDigits[data[i] >> 4]);
      out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
  }

  std::optional<Key> key_from_hex(const std::string &hex) {
    if (hex.size() != kKeySize * 2) {
      return std::nullopt;
    }
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };
    Key key {};
    for (std::size_t i = 0; i < kKeySize; ++i) {
      const int hi = nibble(hex[i * 2]);
      const int lo = nibble(hex[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        return std::nullopt;
      }
      key[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return key;
  }

}  // namespace inputline::link
