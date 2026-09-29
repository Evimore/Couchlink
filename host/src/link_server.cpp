#include "link_server.h"

#include "log.h"
#include "inputline/cpace.h"
#include "inputline/link_client.h"
#include "inputline/sha256.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>

namespace inputline {

  using namespace link;

  namespace {
    constexpr auto kTickInterval = std::chrono::milliseconds(100);
    constexpr auto kNeedAttachInterval = std::chrono::milliseconds(200);
    constexpr std::size_t kMaxRememberedSettings = 32;
    constexpr auto kRecentPairingGrace = std::chrono::seconds(30);

    std::uint64_t random_u64() {
      std::uint8_t bytes[8];
      random_bytes(bytes, sizeof(bytes));
      std::uint64_t value = 0;
      for (auto byte : bytes) {
        value = (value << 8) | byte;
      }
      return value;
    }

    std::string hex_id(std::uint32_t id) {
      char text[9];
      std::snprintf(text, sizeof(text), "%08x", id);
      return text;
    }
  }  // namespace

  void random_bytes(std::uint8_t *out, std::size_t length) {
    // std::random_device is backed by the OS CSPRNG on Windows (rand_s),
    // Linux (getrandom / /dev/urandom) and macOS (arc4random).
    std::random_device device;
    for (std::size_t i = 0; i < length; ++i) {
      out[i] = static_cast<std::uint8_t>(device());
    }
  }

  LinkServer::LinkServer(Options options, ClientStore &store, ControllerBackend &backend):
      options_(std::move(options)),
      store_(store),
      backend_(backend) {}

  LinkServer::~LinkServer() {
    stop();
  }

  bool LinkServer::start() {
    socket_ = net::udp_bind(options_.bind_address, options_.port, &port_);
    if (socket_ == net::kInvalidSocket && options_.bind_address == "::") {
      log::debug("link: IPv6 unavailable, listening on IPv4 only");
      socket_ = net::udp_bind("0.0.0.0", options_.port, &port_);
    }
    if (socket_ == net::kInvalidSocket) {
      log::error("link: cannot bind UDP ", options_.bind_address, ":", options_.port);
      return false;
    }
    running_ = true;
    thread_ = std::thread(&LinkServer::run, this, socket_);
    log::info("link: listening on UDP port ", port_);
    return true;
  }

  void LinkServer::stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    net::close(socket_);
    socket_ = net::kInvalidSocket;

    std::lock_guard lock(mutex_);
    sessions_.clear();  // unplugs every virtual controller
    parked_.clear();
  }

  LinkServer::PairingWindow LinkServer::new_pairing_window(
    const std::string &pin, Clock::duration length, std::function<void(const PairingOutcome &)> on_done, bool remote
  ) {
    PairingWindow window;
    window.pin = pin;
    window.deadline = Clock::now() + length;
    window.on_done = std::move(on_done);
    window.remote = remote;
    return window;
  }

  void LinkServer::open_pairing(const std::string &pin, std::chrono::seconds window, std::function<void(const PairingOutcome &)> on_done) {
    std::lock_guard lock(mutex_);
    pairing_ = new_pairing_window(pin, window, std::move(on_done), false);
    log::info("link: pairing open for ", window.count(), " s");
  }

  ProbeReply LinkServer::probe_reply(std::uint64_t nonce) const {
    ProbeReply reply;
    reply.nonce = nonce;
    reply.pairing_open = pairing_.has_value();
    reply.host_name = options_.host_name;
    reply.min_version = kMinVersion;
    reply.max_version = kVersion;
    reply.software_version = options_.software_version;
    return reply;
  }

  void LinkServer::warn_version(std::uint8_t version, const net::Endpoint &from) {
    const auto now = Clock::now();
    auto &last = version_warned_[from.to_string()];
    if (last != Clock::time_point {} && now - last < std::chrono::minutes(1)) {
      return;
    }
    last = now;
    if (version < kMinVersion) {
      log::warn("link: an older InputLine app (protocol ", int(version), ") at ", from.to_string(), " tried to connect: update the app");
    } else {
      log::warn("link: a newer InputLine app (protocol ", int(version), ") at ", from.to_string(), " tried to connect: update InputLine on this PC");
    }
  }

  bool LinkServer::pairing_open() const {
    std::lock_guard lock(mutex_);
    return pairing_.has_value();
  }

  LinkServer::Status LinkServer::status() const {
    std::lock_guard lock(mutex_);
    Status status = counters_;
    status.sessions = sessions_.size();
    for (const auto &[id, session] : sessions_) {
      status.controllers += session.controllers.size();
      status.clients.push_back(session.client.name);
    }
    return status;
  }

  void LinkServer::process_datagram(const std::uint8_t *data, std::size_t length, const net::Endpoint &from) {
    Deferred deferred;
    {
      std::lock_guard lock(mutex_);
      handle(data, length, from, deferred);
    }
    for (auto &callback : deferred) {
      callback();
    }
  }

  void LinkServer::run(net::Socket socket) {
    std::uint8_t buffer[kMaxDatagram + 1];
    auto next_tick = Clock::now() + kTickInterval;
    while (running_) {
      net::Endpoint from;
      const int received = net::udp_recv(socket, buffer, sizeof(buffer), from, static_cast<int>(kTickInterval.count()));
      Deferred deferred;
      {
        std::lock_guard lock(mutex_);
        if (received > 0) {
          handle(buffer, static_cast<std::size_t>(received), from, deferred);
        }
        const auto now = Clock::now();
        if (now >= next_tick) {
          expire(now, deferred);
          next_tick = now + kTickInterval;
        }
      }
      for (auto &callback : deferred) {
        callback();
      }
    }
  }

  void LinkServer::handle(const std::uint8_t *data, std::size_t length, const net::Endpoint &from, Deferred &deferred) {
    const auto header = peek(data, length);
    if (!header) {
      ++counters_.rejected_datagrams;
      const auto version = wire_version(data, length);
      if (version && *version != kVersion) {
        warn_version(*version, from);
      }
      return;
    }

    switch (key_kind(header->type)) {
      case KeyKind::kNone: {
        const auto datagram = open(data, length, nullptr);
        if (!datagram) {
          ++counters_.rejected_datagrams;
          return;
        }
        if (header->type == Type::kProbe) {
          handle_probe(*datagram, from);
        } else if (header->type == Type::kPairStart) {
          handle_pair_start(*datagram, from, deferred);
        } else if (header->type == Type::kPairRequest) {
          handle_pair_request(*datagram, from, deferred);
        }
        return;
      }
      case KeyKind::kPairing:
        if (header->type == Type::kHello) {
          handle_hello(data, length, from);
        }
        return;
      case KeyKind::kSession:
        handle_session(data, length, from);
        return;
    }
  }

  void LinkServer::handle_probe(const Datagram &datagram, const net::Endpoint &from) {
    // Any version's Probe gets an answer, so the client can say which side to update.
    const auto probe = decode_probe(datagram.payload);
    if (!probe) {
      return;
    }
    if (datagram.header.version != kVersion) {
      warn_version(datagram.header.version, from);
    }
    Header header;
    header.type = Type::kProbeReply;
    send_raw(seal(header, encode(probe_reply(probe->nonce)), nullptr), from);
  }

  void LinkServer::handle_pair_start(const Datagram &datagram, const net::Endpoint &from, Deferred &deferred) {
    const auto start = decode_pair_start(datagram.payload);
    if (!start) {
      return;
    }
    const auto now = Clock::now();
    if (!pairing_ && options_.remote_pairing && now >= remote_pairing_blocked_until_) {
      const auto code = ClientSession::new_pin(random_bytes);
      const auto name = start->client_name.empty() ? std::string("A device") : start->client_name;
      pairing_ = new_pairing_window(code, options_.pairing_window, nullptr, true);
      log::info("link: '", name, "' at ", from.to_string(), " asked to pair; showing a code for ", options_.pairing_window.count(), " s");
      if (options_.show_code) {
        deferred.push_back([show = options_.show_code, name, code] {
          show(name, code);
        });
      }
    }

    auto reply = probe_reply(start->nonce);
    const auto client_id = datagram.header.client_id;
    if (pairing_ && now < pairing_->deadline && client_id != 0) {
      // This attempt's CPace share; the client retransmits, so the same one each time.
      auto &attempts = pairing_->attempts;
      auto it = std::find_if(attempts.begin(), attempts.end(), [&](const PairingWindow::Attempt &a) {
        return a.client_id == client_id && a.nonce == start->nonce;
      });
      if (it == attempts.end()) {
        if (attempts.size() >= kMaxPairingAttempts) {
          attempts.erase(attempts.begin());
        }
        PairingWindow::Attempt attempt;
        attempt.client_id = client_id;
        attempt.nonce = start->nonce;
        random_bytes(attempt.scalar.data(), attempt.scalar.size());
        attempt.share = cpace::public_share(attempt.scalar, pairing_generator(pairing_->pin, client_id, start->nonce));
        attempts.push_back(attempt);
        it = attempts.end() - 1;
      }
      reply.pairing_public_key = it->share;
    }
    Header header;
    header.type = Type::kProbeReply;
    send_raw(seal(header, encode(reply), nullptr), from);
  }

  void LinkServer::handle_pair_request(const Datagram &datagram, const net::Endpoint &from, Deferred &deferred) {
    const auto request = decode_pair_request(datagram.payload);
    const auto client_id = datagram.header.client_id;
    if (!request || client_id == 0) {
      return;
    }

    auto reply_with = [&](bool accepted, const Key &result_key) {
      Header header;
      header.type = Type::kPairResult;
      header.client_id = client_id;
      send_raw(seal(header, encode(PairResult {accepted}), &result_key), from);
    };

    const auto now = Clock::now();
    // The client retransmits until it hears back; repeat a success it may have missed.
    if (recent_pairing_ && now < recent_pairing_->until && recent_pairing_->client_id == client_id &&
        recent_pairing_->client_public_key == request->client_public_key) {
      reply_with(true, recent_pairing_->result_key);
      return;
    }
    if (!pairing_ || now >= pairing_->deadline) {
      return;  // not pairing: stay silent
    }

    auto &attempts = pairing_->attempts;
    const auto it = std::find_if(attempts.begin(), attempts.end(), [&](const PairingWindow::Attempt &a) {
      return a.client_id == client_id && a.nonce == request->nonce;
    });
    if (it == attempts.end()) {
      return;  // no PairStart for this, or already used: stay silent
    }
    // Each CPace secret is used for one exchange only.
    const PairingWindow::Attempt attempt = *it;
    attempts.erase(it);
    const auto keys = derive_pairing_keys(
      attempt.scalar, request->client_public_key, client_id, request->client_public_key, attempt.share, request->nonce, request->client_name
    );
    if (!keys) {
      return;  // invalid share: not a real client
    }
    if (!constant_time_equal(keys->proof.data(), request->proof.data(), keys->proof.size())) {
      ++pairing_->failures;
      log::warn("link: pairing attempt from ", from.to_string(), " used the wrong code (", pairing_->failures, "/", kMaxPairingFailures, ")");
      reply_with(false, pairing_failure_key(client_id, request->client_public_key, attempt.share, request->nonce));
      if (pairing_->failures >= kMaxPairingFailures) {
        if (pairing_->remote) {
          remote_pairing_blocked_until_ = now + kRemotePairingCooldown;
        }
        auto done = std::move(pairing_->on_done);
        pairing_.reset();
        if (done) {
          deferred.push_back([done] {
            done({false, {}});
          });
        }
      }
      return;
    }

    PairedClient client;
    client.client_id = client_id;
    client.key = keys->pairing_key;
    client.name = request->client_name.empty() ? "client-" + hex_id(client_id) : request->client_name;
    store_.upsert(client);
    const bool saved = store_.save();
    if (!saved) {
      log::error("link: paired with '", client.name, "' but could not save ", store_.path());
    }

    reply_with(true, keys->result_key);
    recent_pairing_ = RecentPairing {client_id, request->client_public_key, keys->result_key, now + kRecentPairingGrace};
    log::info("link: paired with '", client.name, "' (", hex_id(client_id), ")");

    auto done = std::move(pairing_->on_done);
    pairing_.reset();
    if (done) {
      deferred.push_back([done, name = client.name] {
        done({true, name});
      });
    }
  }

  void LinkServer::handle_hello(const std::uint8_t *data, std::size_t length, const net::Endpoint &from) {
    const auto header = peek(data, length);
    const auto client = store_.find(header->client_id);
    if (!client) {
      ++counters_.rejected_datagrams;
      return;
    }
    const auto datagram = open(data, length, &client->key);
    if (!datagram) {
      ++counters_.rejected_datagrams;
      return;
    }
    const auto hello = decode_hello(datagram->payload);
    auto &last_counter = last_hello_counter_[client->client_id];
    if (!hello || datagram->header.counter <= last_counter) {
      ++counters_.rejected_datagrams;
      return;  // replayed hello
    }
    last_counter = datagram->header.counter;

    auto existing = sessions_.find(client->client_id);
    if (existing != sessions_.end()) {
      drop_session(existing, "reconnected", true);
    }

    HelloAck ack;
    ack.client_nonce = hello->client_nonce;
    ack.host_nonce = random_u64();
    ack.capabilities = kHostCapSteamController2026;

    Session session;
    session.client = *client;
    session.keys = derive_session_keys(client->key, hello->client_nonce, ack.host_nonce);
    session.endpoint = from;
    session.last_rx = Clock::now();
    sessions_.emplace(client->client_id, std::move(session));

    Header reply;
    reply.type = Type::kHelloAck;
    reply.client_id = client->client_id;
    reply.counter = datagram->header.counter;
    send_raw(seal(reply, encode(ack), &client->key), from);
    log::info("link: '", client->name, "' connected from ", from.to_string(),
              hello->software_version.empty() ? std::string() : " (InputLine " + hello->software_version + ")");
  }

  void LinkServer::handle_session(const std::uint8_t *data, std::size_t length, const net::Endpoint &from) {
    const auto header = peek(data, length);
    auto it = sessions_.find(header->client_id);
    if (it == sessions_.end()) {
      ++counters_.rejected_datagrams;
      return;
    }
    auto &session = it->second;
    const auto datagram = open(data, length, &session.keys.client_to_host);
    if (!datagram || !session.rx_guard.accept(datagram->header.counter)) {
      ++counters_.rejected_datagrams;
      return;
    }
    session.endpoint = from;  // follow the client if its address changes
    session.last_rx = Clock::now();
    if (options_.stats_interval.count() > 0) {
      if (!session.window_started) {
        session.window_started = true;
        session.window_first = datagram->header.counter;
      }
      session.window_last = datagram->header.counter;
      ++session.window_received;
    }

    switch (datagram->header.type) {
      case Type::kInput: {
        const auto input = decode_input(datagram->payload);
        if (!input) {
          return;
        }
        auto *controller = input_target(session, input->controller);
        if (controller != nullptr) {
          record_timing(session, input->controller);
          if (controller->submit(input->report.data(), input->report.size())) {
            ++counters_.reports_forwarded;
          }
        }
        return;
      }

      case Type::kInputBundle: {
        const auto bundle = decode_input_bundle(datagram->payload);
        if (!bundle) {
          return;
        }
        auto *controller = input_target(session, bundle->controller);
        if (controller == nullptr) {
          return;
        }
        const auto seen = session.last_sequence.find(bundle->controller);
        const bool known = seen != session.last_sequence.end();
        const std::uint32_t last = known ? seen->second : 0;
        // Newer than the last report handled? A large step back means the
        // client started counting again (new controller or app restart).
        auto newer = [&](std::uint32_t sequence) {
          const auto ahead = static_cast<std::int32_t>(sequence - last);
          return !known || ahead > 0 || ahead < -1024;
        };
        if (!newer(bundle->sequence)) {
          return;  // duplicate or out of date
        }
        if (known && !bundle->previous.empty() && bundle->sequence - 1 != last && newer(bundle->sequence - 1)) {
          // The datagram carrying the previous report was lost: use the copy.
          if (controller->submit(bundle->previous.data(), bundle->previous.size())) {
            ++counters_.reports_recovered;
            ++session.window_recovered;
            ++counters_.reports_forwarded;
          }
        }
        record_timing(session, bundle->controller);
        if (controller->submit(bundle->report.data(), bundle->report.size())) {
          ++counters_.reports_forwarded;
        }
        session.last_sequence[bundle->controller] = bundle->sequence;
        return;
      }

      case Type::kAttach: {
        const auto attach = decode_attach(datagram->payload);
        if (!attach) {
          return;
        }
        AttachAck ack;
        ack.controller = attach->controller;
        const auto number = attach->controller;
        const auto serial_end = std::find(attach->unit_serial.begin(), attach->unit_serial.end(), '\0');
        const std::string serial(attach->unit_serial.begin(), serial_end);
        std::vector<std::vector<std::uint8_t>> replay;
        if (session.controllers.count(number) == 0) {
          session.last_sequence.erase(number);  // a (re)attached controller numbers its reports afresh
          // The same physical controller coming back takes over its old
          // device, which never left Windows.
          const auto parked = std::find_if(parked_.begin(), parked_.end(), [&](const Parked &p) {
            return p.plugged.route->client_id == session.client.client_id && p.plugged.serial == serial;
          });
          if (parked != parked_.end()) {
            Plugged plugged = std::move(parked->plugged);
            parked_.erase(parked);
            plugged.route->controller = number;
            replay = plugged.route->settings;
            session.controllers.emplace(number, std::move(plugged));
            log::info("link: '", session.client.name, "' controller ", int(number), " is back; it stayed plugged in");
          } else if (session.controllers.size() >= options_.max_controllers_per_session) {
            ack.status = AttachStatus::kAttachFailed;
          } else {
            auto route = std::make_shared<Route>();
            route->client_id = session.client.client_id;
            route->controller = number;
            const auto instance = allocate_instance();
            auto device = backend_.create(*attach, instance, [this, route](OutputKind kind, const std::vector<std::uint8_t> &report) {
              on_output(route, kind, report);
            });
            if (device) {
              session.controllers.emplace(number, Plugged {std::move(device), route, instance, serial});
              log::info("link: '", session.client.name, "' attached controller ", int(number));
            } else {
              ack.status = AttachStatus::kBackendUnavailable;
            }
          }
        }
        send_session(session, Type::kAttachAck, encode(ack));
        // The physical controller may have forgotten Steam's settings (IMU
        // mode...) while it was away; Steam will not send them again.
        for (const auto &report : replay) {
          send_output(session, number, OutputKind::kSetFeature, report);
        }
        return;
      }

      case Type::kDetach: {
        const auto ref = decode_controller_ref(datagram->payload);
        const auto controller = ref ? session.controllers.find(ref->controller) : session.controllers.end();
        if (controller != session.controllers.end()) {
          Plugged plugged = std::move(controller->second);
          session.controllers.erase(controller);
          log::info("link: '", session.client.name, "' lost controller ", int(ref->controller));
          park(std::move(plugged), session.client.name);
        }
        return;
      }

      case Type::kPing: {
        const auto ping = decode_ping(datagram->payload);
        if (ping) {
          send_session(session, Type::kPong, encode(*ping));
        }
        return;
      }

      case Type::kBye:
        drop_session(it, "disconnected", false);  // the stream ended: unplug now
        return;

      default:
        return;
    }
  }

  void LinkServer::expire(Clock::time_point now, Deferred &deferred) {
    if (options_.stats_interval.count() > 0) {
      for (auto &[id, session] : sessions_) {
        if (now - session.timing_logged < options_.stats_interval) {
          continue;
        }
        session.timing_logged = now;
        for (auto &[number, timing] : session.input_timing) {
          if (timing.summary().reports > 0) {
            log::info("stats: '", session.client.name, "' controller ", int(number), " at the PC: ", timing.summary().to_string());
          }
          timing.reset();
        }
        if (session.window_started && session.window_last >= session.window_first) {
          // The client numbers its datagrams consecutively, so gaps in the
          // numbering are datagrams the network lost (or delivered out of order).
          const auto sent = session.window_last - session.window_first + 1;
          const auto lost = sent > session.window_received ? sent - session.window_received : 0;
          char percent[16];
          std::snprintf(percent, sizeof(percent), "%.1f", 100.0 * static_cast<double>(lost) / static_cast<double>(sent));
          log::info("stats: '", session.client.name, "' network: ", lost, " of ", sent, " datagrams lost (", percent, "%), ",
                    session.window_recovered, " lost reports recovered from the next datagram");
        }
        session.window_started = false;
        session.window_received = 0;
        session.window_recovered = 0;
      }
    }

    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if (now - it->second.last_rx > options_.session_timeout) {
        auto next = std::next(it);
        drop_session(it, "timed out", true);
        it = next;
      } else {
        ++it;
      }
    }

    for (auto it = parked_.begin(); it != parked_.end();) {
      if (now >= it->until) {
        log::info("link: controller did not come back; unplugging it");
        it = parked_.erase(it);
      } else {
        ++it;
      }
    }

    if (pairing_ && now >= pairing_->deadline) {
      log::info("link: pairing window closed");
      auto done = std::move(pairing_->on_done);
      pairing_.reset();
      if (done) {
        deferred.push_back([done] {
          done({false, {}});
        });
      }
    }
  }

  void LinkServer::drop_session(std::map<std::uint32_t, Session>::iterator it, const char *reason, bool keep_plugged) {
    log::info("link: '", it->second.client.name, "' ", reason);
    for (auto &[number, plugged] : it->second.controllers) {
      plugged.device->release_all();
      if (keep_plugged) {
        park(std::move(plugged), it->second.client.name);
      }
    }
    sessions_.erase(it);
  }

  void LinkServer::park(Plugged plugged, const std::string &client_name) {
    plugged.device->release_all();
    if (options_.reconnect_grace.count() <= 0) {
      return;  // unplugged when `plugged` goes out of scope
    }
    log::info("link: keeping '", client_name, "' controller plugged in for ", options_.reconnect_grace.count() / 1000, " s in case it comes back");
    parked_.push_back(Parked {std::move(plugged), Clock::now() + options_.reconnect_grace});
  }

  std::uint8_t LinkServer::allocate_instance() const {
    std::set<std::uint8_t> used;
    for (const auto &[id, session] : sessions_) {
      for (const auto &[number, plugged] : session.controllers) {
        used.insert(plugged.instance);
      }
    }
    for (const auto &parked : parked_) {
      used.insert(parked.plugged.instance);
    }
    std::uint8_t instance = 0;
    while (used.count(instance) != 0) {
      ++instance;
    }
    return instance;
  }

  void LinkServer::send_raw(const std::vector<std::uint8_t> &datagram, const net::Endpoint &to) {
    if (!datagram.empty()) {
      net::udp_send(socket_, datagram.data(), datagram.size(), to);
    }
  }

  void LinkServer::send_session(Session &session, Type type, const std::vector<std::uint8_t> &payload) {
    Header header;
    header.type = type;
    header.client_id = session.client.client_id;
    header.counter = ++session.tx_counter;
    send_raw(seal(header, payload, &session.keys.host_to_client), session.endpoint);
  }

  void LinkServer::on_output(const std::shared_ptr<Route> &route, OutputKind kind, const std::vector<std::uint8_t> &report) {
    std::lock_guard lock(mutex_);
    if (report.empty() || report.size() > kMaxReportSize) {
      return;
    }
    if (kind == OutputKind::kSetFeature) {
      auto &settings = route->settings;
      settings.erase(std::remove(settings.begin(), settings.end(), report), settings.end());
      settings.push_back(report);
      if (settings.size() > kMaxRememberedSettings) {
        settings.erase(settings.begin());
      }
    }
    const auto it = sessions_.find(route->client_id);
    if (it == sessions_.end()) {
      return;
    }
    const auto controller = it->second.controllers.find(route->controller);
    if (controller == it->second.controllers.end() || controller->second.route != route) {
      return;  // away (parked): replayed when it comes back
    }
    send_output(it->second, route->controller, kind, report);
  }

  VirtualController *LinkServer::input_target(Session &session, std::uint8_t number) {
    const auto controller = session.controllers.find(number);
    if (controller != session.controllers.end()) {
      return controller->second.device.get();
    }
    // Input for a controller we do not have: ask the client to attach it.
    auto &last = session.last_need_attach[number];
    const auto now = Clock::now();
    if (now - last >= kNeedAttachInterval) {
      last = now;
      send_session(session, Type::kNeedAttach, encode(ControllerRef {number}));
    }
    return nullptr;
  }

  void LinkServer::record_timing(Session &session, std::uint8_t number) {
    if (options_.stats_interval.count() > 0) {
      const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
      session.input_timing[number].add(static_cast<std::uint64_t>(now_us));
    }
  }

  void LinkServer::send_output(Session &session, std::uint8_t controller, OutputKind kind, const std::vector<std::uint8_t> &report) {
    HidOutput output;
    output.controller = controller;
    output.kind = kind;
    output.report = report;
    const auto payload = encode(output);
    send_session(session, Type::kHidOutput, payload);
    if (kind == OutputKind::kSetFeature) {
      // Settings are idempotent and rare; a second copy covers a lost datagram.
      send_session(session, Type::kHidOutput, payload);
    }
    ++counters_.outputs_sent;
  }

}  // namespace inputline
