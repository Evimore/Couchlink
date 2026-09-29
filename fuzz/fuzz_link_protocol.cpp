// Fuzzes the link protocol parsers every datagram from the network reaches
// before authentication (header, unauthenticated messages, the client's
// ProbeReply parser) and every payload decoder, fed directly.

#include "inputline/link_client.h"
#include "inputline/link_protocol.h"
#include "inputline/report_converter.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace inputline;
using namespace inputline::link;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  (void) peek(data, size);
  (void) wire_version(data, size);
  if (const auto datagram = open(data, size, nullptr)) {
    (void) decode_probe(datagram->payload);
    (void) decode_pair_start(datagram->payload);
    (void) decode_pair_request(datagram->payload);
  }
  (void) ClientSession::parse_probe_reply(data, size, 0);

  const std::vector<std::uint8_t> payload(data, data + size);
  (void) decode_probe(payload);
  for (std::uint8_t version = 0; version < 4; ++version) {
    (void) decode_probe_reply(payload, version);
  }
  (void) decode_pair_start(payload);
  (void) decode_pair_request(payload);
  (void) decode_pair_result(payload);
  (void) decode_hello(payload);
  (void) decode_hello_ack(payload);
  (void) decode_attach(payload);
  (void) decode_attach_ack(payload);
  (void) decode_input(payload);
  (void) decode_input_bundle(payload);
  (void) decode_controller_ref(payload);
  (void) decode_ping(payload);
  (void) decode_hid_output(payload);

  // Controller reports from the client are converted on the PC.
  StateReportConverter converter;
  StateReport report {};
  (void) converter.to_wired(data, size, report);
  return 0;
}
