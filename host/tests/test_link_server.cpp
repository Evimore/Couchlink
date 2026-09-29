// End-to-end tests of the link server over real UDP on loopback, with the
// client side played by couchlink::link::ClientSession (the same code the iOS
// client uses) and an in-memory controller backend.

#include "check.h"
#include "client_store.h"
#include "discovery.h"
#include "link_server.h"
#include "log.h"
#include "couchlink/link_client.h"
#include "couchlink/test_pattern.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

using namespace couchlink;
using namespace std::chrono_literals;

namespace {

  void random_fill(std::uint8_t *out, std::size_t length) {
    std::random_device device;
    for (std::size_t i = 0; i < length; ++i) {
      out[i] = static_cast<std::uint8_t>(device());
    }
  }

  struct FakeController;

  struct FakeBackend: ControllerBackend {
    std::mutex mutex;
    int created = 0;
    int destroyed = 0;
    int releases = 0;
    std::vector<std::vector<std::uint8_t>> reports;
    std::vector<std::uint8_t> instances;
    OutputSink last_sink;
    bool fail = false;

    std::unique_ptr<VirtualController> create(const link::Attach &, std::uint8_t instance, OutputSink sink) override;

    int count_created() {
      std::lock_guard lock(mutex);
      return created;
    }

    int count_reports() {
      std::lock_guard lock(mutex);
      return static_cast<int>(reports.size());
    }
  };

  struct FakeController: VirtualController {
    FakeBackend &backend;

    explicit FakeController(FakeBackend &b):
        backend(b) {}

    ~FakeController() override {
      std::lock_guard lock(backend.mutex);
      ++backend.destroyed;
    }

    bool submit(const std::uint8_t *report, std::size_t length) override {
      std::lock_guard lock(backend.mutex);
      backend.reports.emplace_back(report, report + length);
      return true;
    }

    void release_all() override {
      std::lock_guard lock(backend.mutex);
      ++backend.releases;
    }
  };

  std::unique_ptr<VirtualController> FakeBackend::create(const link::Attach &, std::uint8_t instance, OutputSink sink) {
    std::lock_guard lock(mutex);
    if (fail) {
      return nullptr;
    }
    ++created;
    instances.push_back(instance);
    last_sink = std::move(sink);
    return std::make_unique<FakeController>(*this);
  }

  /** A UDP socket standing in for the iPad. */
  struct TestClient {
    net::Socket socket = net::kInvalidSocket;
    net::Endpoint host;

    explicit TestClient(std::uint16_t port) {
      socket = net::udp_bind("127.0.0.1", 0);
      net::resolve("127.0.0.1", port, host);
    }

    ~TestClient() {
      net::close(socket);
    }

    void send(const std::vector<std::uint8_t> &datagram) const {
      net::udp_send(socket, datagram.data(), datagram.size(), host);
    }

    std::vector<std::uint8_t> receive(int timeout_ms = 1000) const {
      std::uint8_t buffer[link::kMaxDatagram + 1];
      net::Endpoint from;
      const int n = net::udp_recv(socket, buffer, sizeof(buffer), from, timeout_ms);
      return n > 0 ? std::vector<std::uint8_t>(buffer, buffer + n) : std::vector<std::uint8_t> {};
    }

    /** Receive until the session yields an event of the given type. */
    std::optional<link::ClientSession::Event> expect(link::ClientSession &session, link::ClientSession::EventType type, int timeout_ms = 1000) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
      while (std::chrono::steady_clock::now() < deadline) {
        const auto datagram = receive(50);
        if (datagram.empty()) {
          continue;
        }
        const auto event = session.handle(datagram.data(), datagram.size());
        if (event && event->type == type) {
          return event;
        }
      }
      return std::nullopt;
    }
  };

  bool eventually(const std::function<bool()> &condition, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (condition()) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return condition();
  }

  std::string temp_config() {
    const auto path = std::filesystem::temp_directory_path() / ("couchlink-test-" + std::to_string(std::random_device {}()) + ".conf");
    return path.string();
  }

  std::uint64_t clock_us() {
    static std::atomic<std::uint64_t> counter {1'000'000};
    return counter += 1000;
  }

  struct Fixture {
    std::string config = temp_config();
    ClientStore store {config};
    FakeBackend backend;

    // Codes the server asked to put on screen (remote pairing).
    std::mutex shown_mutex;
    std::string shown_name;
    std::string shown_code;
    int shown_count = 0;

    LinkServer server;

    explicit Fixture(std::chrono::milliseconds timeout = 3000ms, bool remote_pairing = true, std::chrono::milliseconds grace = 15000ms):
        server(make_options(timeout, remote_pairing, grace), store, backend) {
      CHECK(server.start());
    }

    int shown() {
      std::lock_guard lock(shown_mutex);
      return shown_count;
    }

    ~Fixture() {
      server.stop();
      std::error_code error;
      std::filesystem::remove(config, error);
    }

    LinkServer::Options make_options(std::chrono::milliseconds timeout, bool remote_pairing, std::chrono::milliseconds grace) {
      LinkServer::Options options;
      options.bind_address = "127.0.0.1";
      options.port = 0;
      options.host_name = "Test PC";
      options.session_timeout = timeout;
      options.remote_pairing = remote_pairing;
      options.reconnect_grace = grace;
      options.show_code = [this](const std::string &name, const std::string &code) {
        std::lock_guard lock(shown_mutex);
        shown_name = name;
        shown_code = code;
        ++shown_count;
      };
      return options;
    }

    link::Pairing pair(TestClient &client, const std::string &pin = "424242") {
      std::atomic<int> outcome {-1};
      server.open_pairing(pin, 10s, [&outcome](const LinkServer::PairingOutcome &result) {
        outcome = result.success ? 1 : 0;
      });
      const auto pairing = link::ClientSession::new_pairing(random_fill);
      client.send(link::ClientSession::make_pair_request(pairing, pin, "Test iPad", 1));
      const auto reply = client.receive();
      const auto accepted = link::ClientSession::parse_pair_result(reply.data(), reply.size(), pairing);
      CHECK(accepted.has_value() && *accepted);
      CHECK(eventually([&] {
        return outcome.load() == 1;
      }));
      return pairing;
    }

    bool connect(TestClient &client, link::ClientSession &session) {
      client.send(session.make_hello(clock_us()));
      return client.expect(session, link::ClientSession::EventType::kHelloAck).has_value() && session.established();
    }
  };

  void test_probe_and_pairing() {
    Fixture f;
    TestClient client(f.server.port());

    client.send(link::ClientSession::make_probe(99));
    auto reply = client.receive();
    auto probe = link::ClientSession::parse_probe_reply(reply.data(), reply.size(), 99);
    CHECK(probe && probe->host_name == "Test PC" && !probe->pairing_open);

    // Not pairing: requests are ignored silently.
    const auto stranger = link::ClientSession::new_pairing(random_fill);
    client.send(link::ClientSession::make_pair_request(stranger, "111111", "x", 1));
    CHECK(client.receive(200).empty());

    std::atomic<int> outcome {-1};
    std::string paired_name;
    std::mutex name_mutex;
    f.server.open_pairing("123456", 10s, [&](const LinkServer::PairingOutcome &result) {
      std::lock_guard lock(name_mutex);
      paired_name = result.client_name;
      outcome = result.success ? 1 : 0;
    });
    client.send(link::ClientSession::make_probe(5));
    reply = client.receive();
    probe = link::ClientSession::parse_probe_reply(reply.data(), reply.size(), 5);
    CHECK(probe && probe->pairing_open);

    // Wrong code: authentic rejection, window stays open.
    const auto pairing = link::ClientSession::new_pairing(random_fill);
    client.send(link::ClientSession::make_pair_request(pairing, "654321", "Living room iPad", 7));
    reply = client.receive();
    auto result = link::ClientSession::parse_pair_result(reply.data(), reply.size(), pairing);
    CHECK(result.has_value() && !*result);
    CHECK(f.server.pairing_open());

    // Right code.
    client.send(link::ClientSession::make_pair_request(pairing, "123456", "Living room iPad", 8));
    reply = client.receive();
    result = link::ClientSession::parse_pair_result(reply.data(), reply.size(), pairing);
    CHECK(result.has_value() && *result);
    CHECK(eventually([&] {
      return outcome.load() == 1;
    }));
    {
      std::lock_guard lock(name_mutex);
      CHECK(paired_name == "Living room iPad");
    }
    CHECK(!f.server.pairing_open());

    // A retransmitted request after success is answered again.
    client.send(link::ClientSession::make_pair_request(pairing, "123456", "Living room iPad", 8));
    reply = client.receive();
    result = link::ClientSession::parse_pair_result(reply.data(), reply.size(), pairing);
    CHECK(result.has_value() && *result);

    // Persisted.
    ClientStore reloaded(f.config);
    reloaded.load();
    const auto stored = reloaded.find(pairing.client_id);
    CHECK(stored && stored->key == pairing.key && stored->name == "Living room iPad");
  }

  void test_pairing_lockout() {
    Fixture f;
    TestClient client(f.server.port());
    std::atomic<int> outcome {-1};
    f.server.open_pairing("000000", 10s, [&](const LinkServer::PairingOutcome &result) {
      outcome = result.success ? 1 : 0;
    });
    const auto pairing = link::ClientSession::new_pairing(random_fill);
    for (int i = 0; i < LinkServer::kMaxPairingFailures; ++i) {
      client.send(link::ClientSession::make_pair_request(pairing, "999999", "guesser", static_cast<std::uint64_t>(i)));
      client.receive();
    }
    CHECK(eventually([&] {
      return outcome.load() == 0;
    }));
    CHECK(!f.server.pairing_open());
    client.send(link::ClientSession::make_pair_request(pairing, "000000", "guesser", 100));
    CHECK(client.receive(200).empty());
  }

  std::optional<link::ProbeReply> pair_start(TestClient &client, std::uint64_t nonce, const std::string &name) {
    client.send(link::ClientSession::make_pair_start(nonce, name));
    const auto reply = client.receive();
    return link::ClientSession::parse_probe_reply(reply.data(), reply.size(), nonce);
  }

  void test_remote_pairing() {
    Fixture f;
    TestClient client(f.server.port());

    // An unpaired client asks; the host picks a code and puts it on screen.
    auto reply = pair_start(client, 5, "Couch iPad");
    CHECK(reply && reply->pairing_open && reply->host_name == "Test PC");
    CHECK(eventually([&] {
      return f.shown() == 1;
    }));
    std::string code;
    {
      std::lock_guard lock(f.shown_mutex);
      code = f.shown_code;
      CHECK(f.shown_name == "Couch iPad");
    }
    CHECK(code.size() == link::kPinDigits && code.find_first_not_of("0123456789") == std::string::npos);

    // Asking again while the window is open shows nothing new.
    reply = pair_start(client, 6, "Couch iPad");
    CHECK(reply && reply->pairing_open);
    std::this_thread::sleep_for(50ms);
    CHECK(f.shown() == 1);

    // Typing the code on the client pairs it.
    const auto pairing = link::ClientSession::new_pairing(random_fill);
    client.send(link::ClientSession::make_pair_request(pairing, code, "Couch iPad", 1));
    auto answer = client.receive();
    auto accepted = link::ClientSession::parse_pair_result(answer.data(), answer.size(), pairing);
    CHECK(accepted && *accepted);
    CHECK(eventually([&] {
      return !f.server.pairing_open();
    }));

    // Too many wrong codes close the window and ignore requests for a while.
    reply = pair_start(client, 7, "Stranger");
    CHECK(reply && reply->pairing_open);
    const auto guesser = link::ClientSession::new_pairing(random_fill);
    for (int i = 0; i < LinkServer::kMaxPairingFailures; ++i) {
      client.send(link::ClientSession::make_pair_request(guesser, code == "000000" ? "000001" : "000000", "Stranger", 10 + static_cast<std::uint64_t>(i)));
      client.receive();
    }
    CHECK(eventually([&] {
      return !f.server.pairing_open();
    }));
    const int shown_before = f.shown();
    reply = pair_start(client, 8, "Stranger");
    CHECK(reply && !reply->pairing_open);
    std::this_thread::sleep_for(50ms);
    CHECK(f.shown() == shown_before);
  }

  void test_remote_pairing_disabled() {
    Fixture f(3000ms, false);
    TestClient client(f.server.port());
    const auto reply = pair_start(client, 9, "Couch iPad");
    CHECK(reply && !reply->pairing_open);
    std::this_thread::sleep_for(50ms);
    CHECK(f.shown() == 0);
    CHECK(!f.server.pairing_open());
  }

  void test_session_flow() {
    Fixture f;
    TestClient client(f.server.port());
    const auto pairing = f.pair(client);

    // An unpaired client's hello is ignored.
    link::ClientSession outsider(link::ClientSession::new_pairing(random_fill), "outsider", random_fill);
    client.send(outsider.make_hello(clock_us()));
    CHECK(client.receive(200).empty());

    link::ClientSession session(pairing, "Test iPad", random_fill);
    CHECK(f.connect(client, session));
    CHECK((session.host_capabilities() & link::kHostCapSteamController2026) != 0);

    // Input before attach asks the client to attach.
    TestPattern pattern;
    auto frame = pattern.frame(0);
    client.send(session.make_input(0, frame.data(), frame.size()));
    const auto need = client.expect(session, link::ClientSession::EventType::kNeedAttach);
    CHECK(need && need->need_attach.controller == 0);

    link::Attach attach;
    client.send(session.make_attach(attach));
    const auto ack = client.expect(session, link::ClientSession::EventType::kAttachAck);
    CHECK(ack && ack->attach_ack.status == link::AttachStatus::kOk);
    CHECK(f.backend.count_created() == 1);

    // Attach is idempotent.
    client.send(session.make_attach(attach));
    CHECK(client.expect(session, link::ClientSession::EventType::kAttachAck).has_value());
    CHECK(f.backend.count_created() == 1);

    for (int i = 0; i < 10; ++i) {
      frame = pattern.frame(static_cast<std::uint64_t>(i) * 4000);
      client.send(session.make_input(0, frame.data(), frame.size()));
    }
    CHECK(eventually([&] {
      return f.backend.count_reports() == 10;
    }));
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(!f.backend.reports.empty() && f.backend.reports.back().size() == kBleStateReportSize && f.backend.reports.back()[0] == kReportStateTimestamped);
    }

    // Replayed and tampered datagrams are rejected.
    const auto replayable = session.make_input(0, frame.data(), frame.size());
    client.send(replayable);
    CHECK(eventually([&] {
      return f.backend.count_reports() == 11;
    }));
    client.send(replayable);
    auto tampered = session.make_input(0, frame.data(), frame.size());
    tampered[link::kHeaderSize + 4] ^= 0x01;
    client.send(tampered);
    std::this_thread::sleep_for(100ms);
    CHECK(f.backend.count_reports() == 11);
    CHECK(f.server.status().rejected_datagrams >= 2);

    // Haptics from Steam reach the client; settings are sent twice.
    OutputSink sink;
    {
      std::lock_guard lock(f.backend.mutex);
      sink = f.backend.last_sink;
    }
    CHECK(static_cast<bool>(sink));
    if (sink) {
      sink(link::OutputKind::kOutputReport, {kOutputHapticPulse, 1, 2, 3});
      const auto output = client.expect(session, link::ClientSession::EventType::kHidOutput);
      CHECK(output && output->output.kind == link::OutputKind::kOutputReport && output->output.report[0] == kOutputHapticPulse);

      sink(link::OutputKind::kSetFeature, {1, 0x87, 3, 9, 0, 0});
      const auto first = client.expect(session, link::ClientSession::EventType::kHidOutput);
      const auto second = client.expect(session, link::ClientSession::EventType::kHidOutput);
      CHECK(first && second && first->output.kind == link::OutputKind::kSetFeature && second->output.report == first->output.report);
    }

    client.send(session.make_ping(12345));
    const auto pong = client.expect(session, link::ClientSession::EventType::kPong);
    CHECK(pong && pong->pong.client_time_us == 12345);

    // A lost controller stays plugged in; when it comes back it takes over
    // the same device and gets Steam's settings again.
    const int releases_before = [&] {
      std::lock_guard lock(f.backend.mutex);
      return f.backend.releases;
    }();
    client.send(session.make_detach(0));
    CHECK(eventually([&] {
      return f.server.status().controllers == 0;
    }));
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(f.backend.destroyed == 0 && f.backend.releases > releases_before);
    }
    client.send(session.make_attach(attach));
    CHECK(client.expect(session, link::ClientSession::EventType::kAttachAck).has_value());
    const auto replayed = client.expect(session, link::ClientSession::EventType::kHidOutput);
    CHECK(replayed && replayed->output.kind == link::OutputKind::kSetFeature && replayed->output.report[1] == 0x87);
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(f.backend.created == 1 && f.backend.destroyed == 0);
    }

    // Bye (the stream ended) unplugs right away.
    client.send(session.make_bye());
    CHECK(eventually([&] {
      return f.server.status().sessions == 0;
    }));
    CHECK(eventually([&] {
      std::lock_guard lock(f.backend.mutex);
      return f.backend.destroyed == 1;
    }));
  }

  void test_input_bundle_recovers_lost_reports() {
    Fixture f;
    TestClient client(f.server.port());
    const auto pairing = f.pair(client);
    link::ClientSession session(pairing, "Test iPad", random_fill);
    CHECK(f.connect(client, session));
    link::Attach attach;
    client.send(session.make_attach(attach));
    CHECK(client.expect(session, link::ClientSession::EventType::kAttachAck).has_value());

    TestPattern pattern;
    std::vector<std::array<std::uint8_t, kBleStateReportSize>> frames;
    for (int i = 0; i < 5; ++i) {
      frames.push_back(pattern.frame(static_cast<std::uint64_t>(i) * 4000));
    }
    auto bundle = [&](std::uint32_t sequence) {
      const auto &report = frames[sequence];
      const auto *previous = sequence > 0 ? frames[sequence - 1].data() : nullptr;
      return session.make_input_bundle(0, sequence, report.data(), report.size(), previous, previous ? frames[sequence - 1].size() : 0);
    };

    client.send(bundle(0));
    client.send(bundle(1));
    const auto lost = bundle(2);  // never arrives
    (void) lost;
    client.send(bundle(3));  // carries report 2 as well
    CHECK(eventually([&] {
      return f.backend.count_reports() == 4;
    }));
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(f.backend.reports.size() == 4 && f.backend.reports[2] == std::vector<std::uint8_t>(frames[2].begin(), frames[2].end()));
    }
    CHECK(f.server.status().reports_recovered == 1);

    // An old report arriving late is ignored.
    client.send(session.make_input_bundle(0, 1, frames[1].data(), frames[1].size(), frames[0].data(), frames[0].size()));
    client.send(bundle(4));
    CHECK(eventually([&] {
      return f.backend.count_reports() == 5;
    }));
    std::this_thread::sleep_for(50ms);
    CHECK(f.backend.count_reports() == 5);
  }

  void test_reconnect_replay_and_timeout() {
    Fixture f(300ms, true, 600ms);
    TestClient client(f.server.port());
    const auto pairing = f.pair(client);

    link::ClientSession session(pairing, "Test iPad", random_fill);
    const auto hello = session.make_hello(clock_us());
    client.send(hello);
    CHECK(client.expect(session, link::ClientSession::EventType::kHelloAck).has_value());

    link::Attach attach;
    client.send(session.make_attach(attach));
    CHECK(client.expect(session, link::ClientSession::EventType::kAttachAck).has_value());

    // Replaying the old hello must not reset the session.
    client.send(hello);
    std::this_thread::sleep_for(100ms);
    CHECK(f.server.status().controllers == 1);

    // A fresh hello (app relaunch) replaces the session; its controller
    // stays plugged in, waiting to be claimed again.
    link::ClientSession relaunched(pairing, "Test iPad", random_fill);
    CHECK(f.connect(client, relaunched));
    CHECK(f.server.status().controllers == 0);
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(f.backend.destroyed == 0);
    }

    // Old-session datagrams no longer authenticate.
    TestPattern pattern;
    const auto frame = pattern.frame(0);
    const int before = f.backend.count_reports();
    client.send(session.make_input(0, frame.data(), frame.size()));
    std::this_thread::sleep_for(50ms);
    CHECK(f.backend.count_reports() == before);

    // The relaunched app claims the same device.
    client.send(relaunched.make_attach(attach));
    CHECK(client.expect(relaunched, link::ClientSession::EventType::kAttachAck).has_value());
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(f.backend.created == 1 && f.backend.destroyed == 0);
    }

    // Silence drops the session; its controller is unplugged once the grace period ends.
    CHECK(eventually([&] {
      return f.server.status().sessions == 0;
    }));
    CHECK(eventually([&] {
      std::lock_guard lock(f.backend.mutex);
      return f.backend.destroyed == 1 && f.backend.releases >= 1;
    }));
  }

  void test_backend_failure_and_instances() {
    Fixture f;
    TestClient a(f.server.port());
    TestClient b(f.server.port());
    const auto pa = f.pair(a, "111111");
    const auto pb = f.pair(b, "222222");
    link::ClientSession sa(pa, "A", random_fill);
    link::ClientSession sb(pb, "B", random_fill);
    CHECK(f.connect(a, sa));
    CHECK(f.connect(b, sb));

    link::Attach attach;
    a.send(sa.make_attach(attach));
    CHECK(a.expect(sa, link::ClientSession::EventType::kAttachAck).has_value());
    b.send(sb.make_attach(attach));
    CHECK(b.expect(sb, link::ClientSession::EventType::kAttachAck).has_value());
    {
      std::lock_guard lock(f.backend.mutex);
      CHECK(f.backend.instances.size() == 2 && f.backend.instances[0] != f.backend.instances[1]);
      f.backend.fail = true;
    }
    attach.controller = 1;
    a.send(sa.make_attach(attach));
    const auto ack = a.expect(sa, link::ClientSession::EventType::kAttachAck);
    CHECK(ack && ack->attach_ack.status == link::AttachStatus::kBackendUnavailable);
    CHECK(f.server.status().sessions == 2 && f.server.status().controllers == 2);
  }

  void test_discovery_label() {
    using couchlink::discovery::instance_label;
    CHECK(instance_label("GAMING-PC") == "GAMING-PC");
    CHECK(instance_label("pc.example.lan") == "pc-example-lan");
    CHECK(instance_label("") == "couchlink-host");
    CHECK(instance_label(std::string(80, 'a')).size() == 63);
    // 62 ASCII bytes, then a 2-byte UTF-8 character: cut before it, not inside it.
    CHECK(instance_label(std::string(62, 'a') + "\xC3\xA9") == std::string(62, 'a'));
  }
}  // namespace

int main() {
  net::startup();
  log::set_level(log::Level::kError);
  test_probe_and_pairing();
  test_pairing_lockout();
  test_remote_pairing();
  test_remote_pairing_disabled();
  test_session_flow();
  test_input_bundle_recovers_lost_reports();
  test_reconnect_replay_and_timeout();
  test_backend_failure_and_instances();
  test_discovery_label();
  return test::report_and_exit_code();
}
