// inputline-sim: a stand-in for the InputLine app on an iPad or iPhone.
//
// Pairs with inputline-host and streams a synthetic Steam Controller over the
// real link protocol, so the whole PC side can be tested without iOS.

#include "log.h"
#include "net.h"
#include "inputline/link_client.h"
#include "inputline/test_pattern.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace inputline;
using Clock = std::chrono::steady_clock;

namespace {

  std::atomic<bool> g_quit {false};

  void on_signal(int) {
    g_quit = true;
  }

  void random_fill(std::uint8_t *out, std::size_t length) {
    std::random_device device;
    for (std::size_t i = 0; i < length; ++i) {
      out[i] = static_cast<std::uint8_t>(device());
    }
  }

  std::uint64_t wall_clock_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch()
    )
                                        .count());
  }

  std::uint64_t random_nonce() {
    std::uint8_t bytes[8];
    random_fill(bytes, sizeof(bytes));
    std::uint64_t value = 0;
    for (auto b : bytes) {
      value = (value << 8) | b;
    }
    return value;
  }

  struct Options {
    std::string command = "run";
    std::string host = "127.0.0.1";
    std::uint16_t port = link::kDefaultPort;
    std::string state = "inputline-sim.pairing";
    std::string pin;
    int seconds = 30;
  };

  bool save_pairing(const std::string &path, const link::Pairing &pairing) {
    std::ofstream file(path, std::ios::trunc);
    char id[9];
    std::snprintf(id, sizeof(id), "%08x", pairing.client_id);
    file << id << ' ' << link::to_hex(pairing.key.data(), pairing.key.size()) << '\n';
    return static_cast<bool>(file);
  }

  bool load_pairing(const std::string &path, link::Pairing &pairing) {
    std::ifstream file(path);
    std::string id, key;
    if (!(file >> id >> key)) {
      return false;
    }
    const auto parsed = link::key_from_hex(key);
    if (!parsed) {
      return false;
    }
    pairing.client_id = static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 16));
    pairing.key = *parsed;
    return pairing.client_id != 0;
  }

  struct Link {
    net::Socket socket = net::kInvalidSocket;
    net::Endpoint host;

    bool open(const Options &options) {
      if (!net::resolve(options.host, options.port, host)) {
        std::fprintf(stderr, "cannot resolve %s\n", options.host.c_str());
        return false;
      }
      const bool v6 = host.address.ss_family == AF_INET6;
      socket = net::udp_bind(v6 ? "::" : "0.0.0.0", 0);
      return socket != net::kInvalidSocket;
    }

    void send(const std::vector<std::uint8_t> &datagram) const {
      if (!datagram.empty()) {
        net::udp_send(socket, datagram.data(), datagram.size(), host);
      }
    }

    int receive(std::uint8_t *buffer, std::size_t capacity, int timeout_ms) const {
      net::Endpoint from;
      return net::udp_recv(socket, buffer, capacity, from, timeout_ms);
    }

    ~Link() {
      net::close(socket);
    }
  };

  int probe(const Options &options) {
    Link link;
    if (!link.open(options)) {
      return 1;
    }
    const auto nonce = random_nonce();
    link.send(link::ClientSession::make_probe(nonce));
    std::uint8_t buffer[link::kMaxDatagram];
    const int received = link.receive(buffer, sizeof(buffer), 1000);
    const auto reply = received > 0 ? link::ClientSession::parse_probe_reply(buffer, static_cast<std::size_t>(received), nonce) : std::nullopt;
    if (!reply) {
      std::printf("No inputline-host answered at %s:%u\n", options.host.c_str(), options.port);
      return 1;
    }
    std::printf("Found '%s' (InputLine %s, protocol %d-%d)%s\n", reply->host_name.c_str(),
                reply->software_version.empty() ? "?" : reply->software_version.c_str(), reply->min_version, reply->max_version,
                reply->pairing_open ? ", pairing open" : "");
    switch (link::check_compatibility(*reply)) {
      case link::Compatibility::kUpdateHost:
        std::printf("It is older than this tool: update inputline-host.\n");
        return 1;
      case link::Compatibility::kUpdateClient:
        std::printf("It is newer than this tool: update inputline-sim.\n");
        return 1;
      case link::Compatibility::kCompatible:
        break;
    }
    return 0;
  }

  int pair(const Options &options) {
    Link link;
    if (!link.open(options)) {
      return 1;
    }
    std::uint8_t buffer[link::kMaxDatagram];

    // Ask the host to open pairing; it shows a code on its screen.
    auto pairing = link::ClientSession::begin_pairing(random_fill);
    std::optional<link::ProbeReply> opened;
    for (int attempt = 0; attempt < 5 && !g_quit && !(opened && opened->pairing_open); ++attempt) {
      link.send(link::ClientSession::make_pair_start(pairing, "inputline-sim"));
      const int received = link.receive(buffer, sizeof(buffer), 1000);
      if (received > 0) {
        opened = link::ClientSession::parse_probe_reply(buffer, static_cast<std::size_t>(received), pairing.nonce);
      }
    }
    if (!opened) {
      std::printf("No inputline-host answered at %s:%u\n", options.host.c_str(), options.port);
      return 1;
    }
    if (!opened->pairing_open) {
      std::printf("'%s' is not accepting new devices right now. Run 'inputline-host pair' on it.\n", opened->host_name.c_str());
      return 1;
    }

    std::string pin = options.pin;
    if (pin.empty()) {
      std::printf("Type the code '%s' shows: ", opened->host_name.c_str());
      std::fflush(stdout);
      if (!std::getline(std::cin, pin)) {
        return 1;
      }
    }

    if (link::check_compatibility(*opened) != link::Compatibility::kCompatible) {
      std::printf("'%s' runs an incompatible InputLine (%s): update one of them.\n", opened->host_name.c_str(), opened->software_version.c_str());
      return 1;
    }
    if (!link::ClientSession::enter_pin(pairing, opened->pairing_public_key, pin)) {
      std::printf("'%s' sent an invalid pairing key.\n", opened->host_name.c_str());
      return 1;
    }
    const auto request = link::ClientSession::make_pair_request(pairing, "inputline-sim");
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!g_quit && Clock::now() < deadline) {
      link.send(request);
      const int received = link.receive(buffer, sizeof(buffer), 1000);
      if (received <= 0) {
        continue;
      }
      const auto result = link::ClientSession::parse_pair_result(buffer, static_cast<std::size_t>(received), pairing);
      if (!result) {
        continue;
      }
      if (!*result) {
        std::printf("The host rejected the code.\n");
        return 1;
      }
      if (!save_pairing(options.state, pairing.pairing())) {
        std::fprintf(stderr, "paired, but could not write %s\n", options.state.c_str());
        return 1;
      }
      std::printf("Paired. Saved to %s\n", options.state.c_str());
      return 0;
    }
    std::printf("Timed out waiting for the host.\n");
    return 1;
  }

  int run(const Options &options) {
    link::Pairing pairing;
    if (!load_pairing(options.state, pairing)) {
      std::fprintf(stderr, "no pairing in %s: run 'inputline-sim pair' first\n", options.state.c_str());
      return 1;
    }
    Link link;
    if (!link.open(options)) {
      return 1;
    }
    link::ClientSession session(pairing, "inputline-sim", random_fill);
    std::uint8_t buffer[link::kMaxDatagram];

    // Hello until acknowledged.
    for (int attempt = 0; attempt < 10 && !session.established() && !g_quit; ++attempt) {
      link.send(session.make_hello(wall_clock_us()));
      const int received = link.receive(buffer, sizeof(buffer), 500);
      if (received > 0) {
        session.handle(buffer, static_cast<std::size_t>(received));
      }
    }
    if (!session.established()) {
      std::printf("No answer from %s:%u (is inputline-host running and paired with this client?)\n", options.host.c_str(), options.port);
      return 1;
    }
    std::printf("Connected. Streaming a test pattern for %d s...\n", options.seconds);

    link::Attach attach;
    link.send(session.make_attach(attach));

    TestPattern pattern;
    std::vector<std::uint8_t> previous;
    const auto start = Clock::now();
    auto next_frame = start;
    auto next_ping = start;
    std::uint64_t frames = 0, outputs = 0;
    bool attached = false;
    double last_rtt_ms = -1;

    while (!g_quit && Clock::now() - start < std::chrono::seconds(options.seconds)) {
      const auto now = Clock::now();
      if (now >= next_frame) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        const auto frame = pattern.frame(static_cast<std::uint64_t>(elapsed));
        link.send(session.make_input_bundle(0, static_cast<std::uint32_t>(frames + 1), frame.data(), frame.size(),
                                            previous.empty() ? nullptr : previous.data(), previous.size()));
        previous.assign(frame.begin(), frame.end());
        ++frames;
        next_frame += std::chrono::microseconds(kStateReportIntervalUs);
      }
      if (now >= next_ping) {
        link.send(session.make_ping(wall_clock_us()));
        next_ping = now + std::chrono::seconds(1);
      }

      const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - Clock::now()).count();
      const int received = link.receive(buffer, sizeof(buffer), static_cast<int>(std::max<long long>(0, wait)));
      if (received <= 0) {
        continue;
      }
      const auto event = session.handle(buffer, static_cast<std::size_t>(received));
      if (!event) {
        continue;
      }
      switch (event->type) {
        case link::ClientSession::EventType::kAttachAck:
          attached = event->attach_ack.status == link::AttachStatus::kOk;
          std::printf("Controller %s\n", attached ? "attached" : "attach failed");
          std::fflush(stdout);
          break;
        case link::ClientSession::EventType::kNeedAttach:
          link.send(session.make_attach(attach));
          break;
        case link::ClientSession::EventType::kPong:
          last_rtt_ms = static_cast<double>(wall_clock_us() - event->pong.client_time_us) / 1000.0;
          std::printf("frames %llu  outputs %llu  rtt %.2f ms\n", static_cast<unsigned long long>(frames), static_cast<unsigned long long>(outputs), last_rtt_ms);
          std::fflush(stdout);
          break;
        case link::ClientSession::EventType::kHidOutput:
          if (++outputs <= 20 && !event->output.report.empty()) {
            const auto &report = event->output.report;
            std::printf("host sent %s 0x%02x%s\n",
                        event->output.kind == link::OutputKind::kOutputReport ? "output report" : "setting",
                        event->output.kind == link::OutputKind::kOutputReport ? report[0] : (report.size() > 1 ? report[1] : 0),
                        event->output.kind == link::OutputKind::kOutputReport ? "" : " (feature)");
            std::fflush(stdout);
          }
          break;
        case link::ClientSession::EventType::kHelloAck:
          break;
      }
    }

    link.send(session.make_detach(0));
    link.send(session.make_bye());
    std::printf("Done: %llu frames sent, %llu outputs received, attached=%s\n", static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(outputs), attached ? "yes" : "no");
    return attached ? 0 : 1;
  }

  void usage() {
    std::printf(
      "Usage: inputline-sim [--host H] [--port N] [--state FILE] [--pin CODE] <probe|pair|run [seconds]>\n"
      "  probe   Check that inputline-host is reachable\n"
      "  pair    Pair with inputline-host (type the code it shows, or pass --pin)\n"
      "  run     Stream a synthetic Steam Controller (default 30 s)\n"
    );
  }

}  // namespace

int main(int argc, char **argv) {
  Options options;
  bool have_command = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--host" || arg == "--port" || arg == "--state" || arg == "--pin") && i + 1 < argc) {
      const std::string value = argv[++i];
      if (arg == "--host") {
        options.host = value;
      } else if (arg == "--port") {
        options.port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
      } else if (arg == "--state") {
        options.state = value;
      } else {
        options.pin = value;
      }
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (!have_command) {
      options.command = arg;
      have_command = true;
    } else {
      options.seconds = std::atoi(arg.c_str());
    }
  }

  log::set_level(log::Level::kWarning);
  net::startup();
  std::signal(SIGINT, on_signal);

  if (options.command == "probe") {
    return probe(options);
  }
  if (options.command == "pair") {
    return pair(options);
  }
  if (options.command == "run") {
    return run(options);
  }
  usage();
  return 2;
}
