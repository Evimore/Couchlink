#include "inputline/link_client.h"

namespace inputline::link {

  namespace {
    std::uint64_t random_u64(const RandomSource &random) {
      std::uint8_t bytes[8];
      random(bytes, sizeof(bytes));
      std::uint64_t value = 0;
      for (auto byte : bytes) {
        value = (value << 8) | byte;
      }
      return value;
    }
  }  // namespace

  ClientSession::ClientSession(Pairing pairing, std::string client_name, RandomSource random, std::string software_version):
      pairing_(pairing),
      client_name_(std::move(client_name)),
      random_(std::move(random)),
      software_version_(std::move(software_version)) {}

  std::vector<std::uint8_t> ClientSession::make_probe(std::uint64_t nonce) {
    Header header;
    header.type = Type::kProbe;
    return seal(header, encode(Probe {nonce}), nullptr);
  }

  std::optional<ProbeReply> ClientSession::parse_probe_reply(const std::uint8_t *data, std::size_t length, std::uint64_t expected_nonce) {
    const auto datagram = open(data, length, nullptr);
    if (!datagram || datagram->header.type != Type::kProbeReply) {
      return std::nullopt;
    }
    auto reply = decode_probe_reply(datagram->payload, datagram->header.version);
    if (!reply || reply->nonce != expected_nonce) {
      return std::nullopt;
    }
    return reply;
  }

  std::vector<std::uint8_t> ClientSession::make_pair_start(const PairingAttempt &attempt, const std::string &client_name) {
    Header header;
    header.type = Type::kPairStart;
    return seal(header, encode(PairStart {attempt.nonce, client_name}), nullptr);
  }

  ClientSession::PairingAttempt ClientSession::begin_pairing(const RandomSource &random) {
    PairingAttempt attempt;
    std::uint8_t id[4];
    do {
      random(id, sizeof(id));
      attempt.client_id = (static_cast<std::uint32_t>(id[0]) << 24) | (static_cast<std::uint32_t>(id[1]) << 16) |
                          (static_cast<std::uint32_t>(id[2]) << 8) | id[3];
    } while (attempt.client_id == 0);
    attempt.nonce = random_u64(random);
    random(attempt.secret.data(), attempt.secret.size());
    attempt.public_key = crypto::x25519_public_key(attempt.secret);
    return attempt;
  }

  bool ClientSession::enter_pin(PairingAttempt &attempt, const Key &host_public_key, const std::string &pin) {
    const auto keys = derive_pairing_keys(
      attempt.secret, host_public_key, attempt.client_id, attempt.public_key, host_public_key, attempt.nonce, pin
    );
    attempt.keys_ready = keys.has_value();
    if (keys) {
      attempt.keys = *keys;
    }
    return attempt.keys_ready;
  }

  std::string ClientSession::new_pin(const RandomSource &random) {
    std::string pin;
    while (pin.size() < kPinDigits) {
      std::uint8_t byte = 0;
      random(&byte, 1);
      if (byte < 250) {  // reject to avoid modulo bias
        pin.push_back(static_cast<char>('0' + byte % 10));
      }
    }
    return pin;
  }

  std::vector<std::uint8_t> ClientSession::make_pair_request(const PairingAttempt &attempt, const std::string &client_name) {
    if (!attempt.keys_ready) {
      return {};
    }
    PairRequest request;
    request.client_public_key = attempt.public_key;
    request.nonce = attempt.nonce;
    request.client_name = client_name;
    request.proof = attempt.keys.proof;

    Header header;
    header.type = Type::kPairRequest;
    header.client_id = attempt.client_id;
    return seal(header, encode(request), nullptr);
  }

  std::optional<bool> ClientSession::parse_pair_result(const std::uint8_t *data, std::size_t length, const PairingAttempt &attempt) {
    if (!attempt.keys_ready) {
      return std::nullopt;
    }
    const auto datagram = open(data, length, &attempt.keys.result_key);
    if (!datagram || datagram->header.type != Type::kPairResult || datagram->header.client_id != attempt.client_id) {
      return std::nullopt;
    }
    const auto result = decode_pair_result(datagram->payload);
    if (!result) {
      return std::nullopt;
    }
    return result->accepted;
  }

  std::vector<std::uint8_t> ClientSession::make_hello(std::uint64_t now_us) {
    established_ = false;
    client_nonce_ = random_u64(random_);

    Header header;
    header.type = Type::kHello;
    header.client_id = pairing_.client_id;
    header.counter = now_us;
    return seal(header, encode(Hello {client_nonce_, client_name_, software_version_}), &pairing_.key);
  }

  std::vector<std::uint8_t> ClientSession::seal_session(Type type, const std::vector<std::uint8_t> &payload) {
    if (!established_) {
      return {};
    }
    Header header;
    header.type = type;
    header.client_id = pairing_.client_id;
    header.counter = ++tx_counter_;
    return seal(header, payload, &session_keys_.client_to_host);
  }

  std::vector<std::uint8_t> ClientSession::make_attach(const Attach &attach) {
    return seal_session(Type::kAttach, encode(attach));
  }

  std::vector<std::uint8_t> ClientSession::make_input(std::uint8_t controller, const std::uint8_t *report, std::size_t length) {
    if (report == nullptr || length == 0 || length > kMaxReportSize) {
      return {};
    }
    Input input;
    input.controller = controller;
    input.report.assign(report, report + length);
    return seal_session(Type::kInput, encode(input));
  }

  std::vector<std::uint8_t> ClientSession::make_input_bundle(std::uint8_t controller, std::uint32_t sequence, const std::uint8_t *report, std::size_t length,
                                                             const std::uint8_t *previous, std::size_t previous_length) {
    if (report == nullptr || length == 0 || length > kMaxReportSize || previous_length > kMaxReportSize) {
      return {};
    }
    InputBundle bundle;
    bundle.controller = controller;
    bundle.sequence = sequence;
    bundle.report.assign(report, report + length);
    if (previous != nullptr && previous_length > 0) {
      bundle.previous.assign(previous, previous + previous_length);
    }
    return seal_session(Type::kInputBundle, encode(bundle));
  }

  std::vector<std::uint8_t> ClientSession::make_detach(std::uint8_t controller) {
    return seal_session(Type::kDetach, encode(ControllerRef {controller}));
  }

  std::vector<std::uint8_t> ClientSession::make_ping(std::uint64_t client_time_us) {
    return seal_session(Type::kPing, encode(Ping {client_time_us}));
  }

  std::vector<std::uint8_t> ClientSession::make_bye() {
    return seal_session(Type::kBye, {});
  }

  std::optional<ClientSession::Event> ClientSession::handle(const std::uint8_t *data, std::size_t length) {
    const auto header = peek(data, length);
    if (!header || header->client_id != pairing_.client_id) {
      return std::nullopt;
    }

    if (header->type == Type::kHelloAck) {
      const auto datagram = open(data, length, &pairing_.key);
      if (!datagram) {
        return std::nullopt;
      }
      const auto ack = decode_hello_ack(datagram->payload);
      if (!ack || ack->client_nonce != client_nonce_) {
        return std::nullopt;  // stale or replayed ack
      }
      session_keys_ = derive_session_keys(pairing_.key, client_nonce_, ack->host_nonce);
      capabilities_ = ack->capabilities;
      tx_counter_ = 0;
      rx_guard_.reset();
      established_ = true;
      return Event {EventType::kHelloAck, {}, {}, {}, {}};
    }

    if (!established_ || key_kind(header->type) != KeyKind::kSession) {
      return std::nullopt;
    }
    const auto datagram = open(data, length, &session_keys_.host_to_client);
    if (!datagram || !rx_guard_.accept(datagram->header.counter)) {
      return std::nullopt;
    }

    Event event;
    switch (datagram->header.type) {
      case Type::kAttachAck: {
        const auto ack = decode_attach_ack(datagram->payload);
        if (!ack) {
          return std::nullopt;
        }
        event.type = EventType::kAttachAck;
        event.attach_ack = *ack;
        return event;
      }
      case Type::kNeedAttach: {
        const auto ref = decode_controller_ref(datagram->payload);
        if (!ref) {
          return std::nullopt;
        }
        event.type = EventType::kNeedAttach;
        event.need_attach = *ref;
        return event;
      }
      case Type::kPong: {
        const auto pong = decode_ping(datagram->payload);
        if (!pong) {
          return std::nullopt;
        }
        event.type = EventType::kPong;
        event.pong = *pong;
        return event;
      }
      case Type::kHidOutput: {
        auto output = decode_hid_output(datagram->payload);
        if (!output) {
          return std::nullopt;
        }
        event.type = EventType::kHidOutput;
        event.output = std::move(*output);
        return event;
      }
      default:
        return std::nullopt;
    }
  }

}  // namespace inputline::link
