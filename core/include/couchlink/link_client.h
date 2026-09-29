/**
 * @file link_client.h
 * @brief Client side of the link protocol, without any I/O.
 *
 * Builds outgoing datagrams and interprets incoming ones. The Couchlink app
 * and the couchlink-sim test tool both drive this class; each owns its own
 * socket.
 */
#pragma once

#include "link_protocol.h"

#include <functional>
#include <optional>

namespace couchlink::link {

  /** Fills a buffer with cryptographically secure random bytes. */
  using RandomSource = std::function<void(std::uint8_t *out, std::size_t length)>;

  /** A pairing the client remembers for one host. */
  struct Pairing {
    std::uint32_t client_id = 0;
    Key key {};
  };

  class ClientSession {
  public:
    ClientSession(Pairing pairing, std::string client_name, RandomSource random);

    // ---- Discovery and pairing (static: no session needed) -------------------

    static std::vector<std::uint8_t> make_probe(std::uint64_t nonce);
    static std::optional<ProbeReply> parse_probe_reply(const std::uint8_t *data, std::size_t length, std::uint64_t expected_nonce);

    /** Generate a new pairing (random client ID and key). */
    static Pairing new_pairing(const RandomSource &random);

    /** Generate a random 6-digit pairing code (the host shows it, the user types it on the client). */
    static std::string new_pin(const RandomSource &random);

    /** Ask the host to open pairing and show a code; it answers with a ProbeReply. */
    static std::vector<std::uint8_t> make_pair_start(std::uint64_t nonce, const std::string &client_name);

    static std::vector<std::uint8_t> make_pair_request(const Pairing &pairing, const std::string &pin,
                                                       const std::string &client_name, std::uint64_t nonce);

    /** @return true/false for an authentic PairResult, nullopt for anything else. */
    static std::optional<bool> parse_pair_result(const std::uint8_t *data, std::size_t length, const Pairing &pairing);

    // ---- Session ------------------------------------------------------------

    /**
     * @brief Start (or restart) a session.
     * @param now_us Monotonic-ish wall clock in microseconds; hello counters
     *               must increase across app launches so the host can reject replays.
     */
    std::vector<std::uint8_t> make_hello(std::uint64_t now_us);

    bool established() const {
      return established_;
    }

    std::uint32_t host_capabilities() const {
      return capabilities_;
    }

    std::vector<std::uint8_t> make_attach(const Attach &attach);
    std::vector<std::uint8_t> make_input(std::uint8_t controller, const std::uint8_t *report, std::size_t length);
    /** Report @p sequence plus the report before it (@p previous may be empty). */
    std::vector<std::uint8_t> make_input_bundle(std::uint8_t controller, std::uint32_t sequence, const std::uint8_t *report, std::size_t length,
                                                const std::uint8_t *previous, std::size_t previous_length);
    std::vector<std::uint8_t> make_detach(std::uint8_t controller);
    std::vector<std::uint8_t> make_ping(std::uint64_t client_time_us);
    std::vector<std::uint8_t> make_bye();

    enum class EventType {
      kHelloAck,
      kAttachAck,
      kNeedAttach,
      kPong,
      kHidOutput,
    };

    struct Event {
      EventType type = EventType::kHelloAck;
      AttachAck attach_ack;
      ControllerRef need_attach;
      Ping pong;
      HidOutput output;
    };

    /** Authenticate and interpret one datagram from the host. */
    std::optional<Event> handle(const std::uint8_t *data, std::size_t length);

  private:
    std::vector<std::uint8_t> seal_session(Type type, const std::vector<std::uint8_t> &payload);

    Pairing pairing_;
    std::string client_name_;
    RandomSource random_;

    bool established_ = false;
    std::uint64_t client_nonce_ = 0;
    std::uint32_t capabilities_ = 0;
    Key session_key_ {};
    std::uint64_t tx_counter_ = 0;
    ReplayGuard rx_guard_;
  };

}  // namespace couchlink::link
