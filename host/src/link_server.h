/**
 * @file link_server.h
 * @brief The UDP endpoint clients stream controller reports to.
 *
 * One thread receives datagrams, authenticates them, and turns them into
 * virtual controller operations. Haptics and settings Steam sends to a
 * virtual controller are routed back to the client that owns it.
 */
#pragma once

#include "client_store.h"
#include "controller_backend.h"
#include "net.h"
#include "inputline/link_protocol.h"
#include "inputline/timing_stats.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace inputline {

  class LinkServer {
  public:
    using Clock = std::chrono::steady_clock;

    struct Options {
      std::string bind_address = "::";
      std::uint16_t port = link::kDefaultPort;
      std::string host_name = "inputline-host";
      /** inputline-host's version, told to clients so they can say which side to update. */
      std::string software_version;
      std::chrono::milliseconds session_timeout {3000};
      std::size_t max_controllers_per_session = 4;

      /**
       * How long a virtual controller stays plugged in after its client lost
       * it (Bluetooth hiccup, Wi-Fi drop, app restart). If the same controller
       * comes back in time it takes over the same USB device, so Windows and
       * Steam never see it leave.
       */
      std::chrono::milliseconds reconnect_grace {120000};

      /** Log report timing per controller this often (0 = off). */
      std::chrono::seconds stats_interval {0};

      /**
       * Let an unpaired client ask for pairing: the host picks a code and
       * passes it to show_code, which puts it on the PC's screen. The user
       * sees it through the stream and types it on the client.
       */
      bool remote_pairing = true;
      std::chrono::seconds pairing_window {120};
      /** Called outside the server lock with the client's name and the code. */
      std::function<void(const std::string &client_name, const std::string &code)> show_code;
    };

    struct PairingOutcome {
      bool success = false;
      std::string client_name;
    };

    struct Status {
      std::size_t sessions = 0;
      std::size_t controllers = 0;
      std::uint64_t reports_forwarded = 0;
      std::uint64_t outputs_sent = 0;
      std::uint64_t rejected_datagrams = 0;
      std::uint64_t reports_recovered = 0;  ///< lost reports rebuilt from the next datagram
    };

    LinkServer(Options options, ClientStore &store, ControllerBackend &backend);
    ~LinkServer();

    LinkServer(const LinkServer &) = delete;
    LinkServer &operator=(const LinkServer &) = delete;

    bool start();
    void stop();
    std::uint16_t port() const {
      return port_;
    }

    /**
     * @brief Accept one new pairing for a limited time.
     * @param pin The code the user types on the client.
     * @param on_done Called once, from the server thread, with the result.
     */
    void open_pairing(const std::string &pin, std::chrono::seconds window, std::function<void(const PairingOutcome &)> on_done);
    bool pairing_open() const;

    Status status() const;

    static constexpr int kMaxPairingFailures = 5;
    /** After a remotely started pairing is locked out, ignore new requests this long. */
    static constexpr std::chrono::minutes kRemotePairingCooldown {5};

  private:
    /** Where a virtual controller's haptics and settings go; changes when it is handed over. */
    struct Route {
      std::uint32_t client_id = 0;
      std::uint8_t controller = 0;
      /** Settings Steam wrote, replayed to the physical controller after a reconnect. */
      std::vector<std::vector<std::uint8_t>> settings;
    };

    struct Plugged {
      std::unique_ptr<VirtualController> device;
      std::shared_ptr<Route> route;
      std::uint8_t instance = 0;
      std::string serial;  ///< identifies the physical controller when it comes back
    };

    struct Parked {
      Plugged plugged;
      Clock::time_point until;
    };

    struct Session {
      PairedClient client;
      link::SessionKeys keys;
      net::Endpoint endpoint;
      std::uint64_t tx_counter = 0;
      link::ReplayGuard rx_guard;
      Clock::time_point last_rx;
      std::map<std::uint8_t, Plugged> controllers;
      std::map<std::uint8_t, TimingStats> input_timing;
      std::map<std::uint8_t, std::uint32_t> last_sequence;  ///< newest InputBundle report per controller
      Clock::time_point timing_logged;
      // Datagram counters seen in the current stats window, to estimate loss.
      bool window_started = false;
      std::uint64_t window_first = 0;
      std::uint64_t window_last = 0;
      std::uint64_t window_received = 0;
      std::uint64_t window_recovered = 0;
      std::map<std::uint8_t, Clock::time_point> last_need_attach;
    };

    struct PairingWindow {
      std::string pin;
      Clock::time_point deadline;
      int failures = 0;
      std::function<void(const PairingOutcome &)> on_done;
      bool remote = false;  ///< opened by a client's PairStart
      link::Key secret {};  ///< temporary X25519 key for this window only
      link::Key public_key {};
    };

    struct RecentPairing {
      std::uint32_t client_id = 0;
      link::Key client_public_key {};
      link::Key result_key {};
      Clock::time_point until;
    };

    PairingWindow new_pairing_window(const std::string &pin, Clock::duration length, std::function<void(const PairingOutcome &)> on_done, bool remote);
    link::ProbeReply probe_reply(std::uint64_t nonce) const;
    void warn_version(std::uint8_t version, const net::Endpoint &from);
    std::map<std::string, Clock::time_point> version_warned_;

    using Deferred = std::vector<std::function<void()>>;

    void run(net::Socket socket);
    void handle(const std::uint8_t *data, std::size_t length, const net::Endpoint &from, Deferred &deferred);
    void handle_probe(const link::Datagram &datagram, const net::Endpoint &from);
    void handle_pair_start(const link::Datagram &datagram, const net::Endpoint &from, Deferred &deferred);
    void handle_pair_request(const link::Datagram &datagram, const net::Endpoint &from, Deferred &deferred);
    void handle_hello(const std::uint8_t *data, std::size_t length, const net::Endpoint &from);
    void handle_session(const std::uint8_t *data, std::size_t length, const net::Endpoint &from);
    void expire(Clock::time_point now, Deferred &deferred);

    void send_raw(const std::vector<std::uint8_t> &datagram, const net::Endpoint &to);
    void send_session(Session &session, link::Type type, const std::vector<std::uint8_t> &payload);
    void on_output(const std::shared_ptr<Route> &route, link::OutputKind kind, const std::vector<std::uint8_t> &report);
    std::uint8_t allocate_instance() const;
    void drop_session(std::map<std::uint32_t, Session>::iterator it, const char *reason, bool keep_plugged);
    void park(Plugged plugged, const std::string &client_name);
    /** The controller input is for, or nullptr (and a NeedAttach) if it is not attached. */
    VirtualController *input_target(Session &session, std::uint8_t number);
    void record_timing(Session &session, std::uint8_t number);
    void send_output(Session &session, std::uint8_t controller, link::OutputKind kind, const std::vector<std::uint8_t> &report);

    Options options_;
    ClientStore &store_;
    ControllerBackend &backend_;

    mutable std::mutex mutex_;
    std::map<std::uint32_t, Session> sessions_;
    std::vector<Parked> parked_;
    std::map<std::uint32_t, std::uint64_t> last_hello_counter_;
    std::optional<PairingWindow> pairing_;
    std::optional<RecentPairing> recent_pairing_;
    Clock::time_point remote_pairing_blocked_until_ {};
    Status counters_;

    net::Socket socket_ = net::kInvalidSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_ {false};
    std::thread thread_;
  };

  /** Fill a buffer with cryptographically secure random bytes. */
  void random_bytes(std::uint8_t *out, std::size_t length);

}  // namespace inputline
