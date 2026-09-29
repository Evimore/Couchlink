// Fuzzes the USB/IP server. It listens on 127.0.0.1 only, but any program on
// the PC can connect to it while InputLine runs as a service, so it must
// survive anything. The input is what a client sends over one connection.
// The first byte picks how it starts:
//   0: as is
//   1: a valid OP_REQ_IMPORT of device 1-1, so the rest is URBs to the
//      virtual Steam Controller (descriptors, Steam's feature reports...)
//   2: a valid OP_REQ_DEVLIST

#include "log.h"
#include "net.h"
#include "steam_controller_device.h"
#include "usbip_server.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace inputline;

namespace {
  struct Harness {
    usbip::Server server;

    Harness() {
      log::set_level(log::Level::kError);
      server.add_device(std::make_shared<SteamControllerDevice>());
    }
  };

  Harness &harness() {
    static Harness instance;
    return instance;
  }
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  Harness &h = harness();
  if (size < 1) {
    return 0;
  }
  std::vector<std::uint8_t> stream;
  const std::uint8_t mode = data[0] % 3;
  if (mode != 0) {
    const std::uint16_t code = mode == 1 ? usbip::proto::kOpReqImport : usbip::proto::kOpReqDevlist;
    stream = {0x01, 0x11, static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code), 0, 0, 0, 0};
    if (mode == 1) {
      const char busid[32] = "1-1";
      stream.insert(stream.end(), busid, busid + sizeof(busid));
    }
  }
  stream.insert(stream.end(), data + 1, data + size);
  data = stream.data();
  size = stream.size();

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return 0;
  }
  // Replies stay small for inputs up to libFuzzer's default length, so
  // they fit in the socket buffer without a reader.
  std::size_t written = 0;
  while (written < size) {
    const ssize_t n = write(fds[1], data + written, size - written);
    if (n <= 0) {
      break;
    }
    written += static_cast<std::size_t>(n);
  }
  shutdown(fds[1], SHUT_WR);
  h.server.serve_connection(fds[0]);
  close(fds[0]);
  close(fds[1]);
  return 0;
}
