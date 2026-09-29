// Fuzzes the PC's link server, the part of InputLine reachable from the
// network. The first input byte picks what the rest is:
//   0: a raw datagram from anyone (no keys needed)
//   1: a session message from a paired client, correctly sealed, so the
//      decoders and session logic behind authentication are reached too:
//      byte 1 is the message type, the rest its payload.
//   2: a well-formed Attach with fuzzed fields, then Inputs.

#include "client_store.h"
#include "controller_backend.h"
#include "link_server.h"
#include "log.h"
#include "net.h"
#include "inputline/link_client.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace inputline;

namespace {

  struct NullController: VirtualController {
    bool submit(const std::uint8_t *, std::size_t) override {
      return true;
    }
    void release_all() override {}
  };

  struct NullBackend: ControllerBackend {
    std::unique_ptr<VirtualController> create(const link::Attach &, std::uint8_t, OutputSink) override {
      return std::make_unique<NullController>();
    }
  };

  void fill(std::uint8_t *out, std::size_t length) {
    static std::uint8_t counter = 0;
    for (std::size_t i = 0; i < length; ++i) {
      out[i] = ++counter;
    }
  }

  struct Harness {
    ClientStore store;
    NullBackend backend;
    std::unique_ptr<LinkServer> server;
    net::Socket client_socket = net::kInvalidSocket;
    net::Endpoint client_endpoint;
    std::unique_ptr<link::ClientSession> session;
    std::uint64_t clock_us = 1'000'000;

    Harness() {
      net::startup();
      log::set_level(log::Level::kError);
      link::Pairing pairing;
      pairing.client_id = 0x12345678;
      pairing.key.fill(0x42);
      store.upsert(PairedClient {pairing.client_id, pairing.key, "fuzz"});

      LinkServer::Options options;
      options.bind_address = "127.0.0.1";
      options.port = 0;
      options.host_name = "fuzz-host";
      options.remote_pairing = true;
      options.show_code = [](const std::string &, const std::string &) {};
      server = std::make_unique<LinkServer>(options, store, backend);
      server->start();

      std::uint16_t client_port = 0;
      client_socket = net::udp_bind("127.0.0.1", 0, &client_port);
      net::resolve("127.0.0.1", client_port, client_endpoint);
      session = std::make_unique<link::ClientSession>(pairing, "fuzz", fill, "0.0.0");
      connect();
    }

    /** Hello, then read the HelloAck the server sent to our socket. */
    void connect() {
      const auto hello = session->make_hello(clock_us += 1000);
      server->process_datagram(hello.data(), hello.size(), client_endpoint);
      std::uint8_t buffer[link::kMaxDatagram + 1];
      for (int i = 0; i < 20 && !session->established(); ++i) {
        net::Endpoint from;
        const int n = net::udp_recv(client_socket, buffer, sizeof(buffer), from, 100);
        if (n > 0) {
          (void) session->handle(buffer, static_cast<std::size_t>(n));
        }
      }
    }

    void drain() {
      std::uint8_t buffer[link::kMaxDatagram + 1];
      net::Endpoint from;
      while (net::udp_recv(client_socket, buffer, sizeof(buffer), from, 0) > 0) {
      }
    }

    void feed(const std::vector<std::uint8_t> &datagram) {
      server->process_datagram(datagram.data(), datagram.size(), client_endpoint);
    }
  };

  Harness &harness() {
    static Harness instance;
    return instance;
  }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  Harness &h = harness();
  if (!h.session->established()) {
    h.connect();
  }
  if (size < 1) {
    return 0;
  }
  const std::uint8_t mode = data[0] % 3;
  ++data;
  --size;
  if (mode == 0) {
    h.server->process_datagram(data, size, h.client_endpoint);
  } else if (mode == 1 && size >= 1) {
    const std::vector<std::uint8_t> payload(data + 1, data + size);
    h.feed(h.session->make_raw(static_cast<link::Type>(data[0]), payload));
  } else if (mode == 2 && size >= 4) {
    link::Attach attach;
    attach.controller = data[0] & 3;
    attach.kind = static_cast<link::DeviceKind>(data[1]);
    attach.transport = static_cast<link::Transport>(data[2]);
    data += 2;
    size -= 2;
    const std::size_t half = (size - 1) / 2;
    std::memcpy(attach.attributes_reply.data(), data + 1, std::min(half, attach.attributes_reply.size()));
    std::memcpy(attach.unit_serial.data(), data + 1, std::min(half, attach.unit_serial.size()));
    h.feed(h.session->make_attach(attach));
    h.feed(h.session->make_input(attach.controller, data + 1 + half, size - 1 - half));
    h.feed(h.session->make_input_bundle(attach.controller, static_cast<std::uint32_t>(size), data + 1 + half, size - 1 - half, data + 1, half));
  }
  h.drain();
  return 0;
}
