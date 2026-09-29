/**
 * @file usbip_test_client.h
 * @brief Minimal USB/IP client (the role usbip-win2 / vhci-hcd play) for tests.
 */
#pragma once

#include "net.h"
#include "usbip_server.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inputline::test {

  struct RetSubmit {
    std::uint32_t seqnum = 0;
    std::int32_t status = 0;
    std::int32_t actual_length = 0;
    std::vector<std::uint8_t> data;
  };

  struct DevlistEntry {
    std::string busid;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    std::uint8_t num_interfaces = 0;
    std::uint8_t interface_class = 0;
  };

  class UsbipTestClient {
  public:
    explicit UsbipTestClient(std::uint16_t port):
        port_(port) {}

    ~UsbipTestClient() {
      net::close(socket_);
    }

    std::vector<DevlistEntry> devlist() {
      std::vector<DevlistEntry> result;
      const auto socket = net::tcp_connect("127.0.0.1", port_);
      if (socket == net::kInvalidSocket) {
        return result;
      }
      auto request = op_header(usbip::proto::kOpReqDevlist);
      net::send_all(socket, request.data(), request.size());
      std::uint8_t header[12];
      if (net::recv_all(socket, header, sizeof(header))) {
        const auto count = be32(header + 8);
        for (std::uint32_t i = 0; i < count; ++i) {
          std::vector<std::uint8_t> device(usbip::proto::kUsbDeviceSize);
          if (!net::recv_all(socket, device.data(), device.size())) {
            break;
          }
          DevlistEntry entry;
          entry.busid.assign(reinterpret_cast<const char *>(device.data() + 256));
          entry.vendor = be16(device.data() + 300);
          entry.product = be16(device.data() + 302);
          entry.num_interfaces = device[311];
          std::vector<std::uint8_t> interfaces(entry.num_interfaces * 4U);
          net::recv_all(socket, interfaces.data(), interfaces.size());
          entry.interface_class = interfaces.empty() ? 0 : interfaces[0];
          result.push_back(entry);
        }
      }
      net::close(socket);
      return result;
    }

    /** Import a device; afterwards this client carries its URBs. */
    bool import(const std::string &busid, std::uint16_t *vendor = nullptr, std::uint16_t *product = nullptr) {
      socket_ = net::tcp_connect("127.0.0.1", port_);
      if (socket_ == net::kInvalidSocket) {
        return false;
      }
      auto request = op_header(usbip::proto::kOpReqImport);
      char bus[32] = {};
      std::snprintf(bus, sizeof(bus), "%s", busid.c_str());
      request.insert(request.end(), bus, bus + sizeof(bus));
      net::send_all(socket_, request.data(), request.size());

      std::uint8_t header[8];
      if (!net::recv_all(socket_, header, sizeof(header)) || be32(header + 4) != 0) {
        return false;
      }
      std::vector<std::uint8_t> device(usbip::proto::kUsbDeviceSize);
      if (!net::recv_all(socket_, device.data(), device.size())) {
        return false;
      }
      if (vendor) {
        *vendor = be16(device.data() + 300);
      }
      if (product) {
        *product = be16(device.data() + 302);
      }
      return true;
    }

    /** Send CMD_SUBMIT without waiting for the reply. */
    std::uint32_t submit(std::uint32_t direction, std::uint32_t endpoint, std::uint32_t buffer_length,
                         const std::uint8_t setup[8] = nullptr, const std::vector<std::uint8_t> &out = {}) {
      const auto seqnum = ++seqnum_;
      std::vector<std::uint8_t> cmd;
      put32(cmd, usbip::proto::kCmdSubmit);
      put32(cmd, seqnum);
      put32(cmd, 0x00010001);
      put32(cmd, direction);
      put32(cmd, endpoint);
      put32(cmd, 0);  // transfer_flags
      put32(cmd, buffer_length);
      put32(cmd, 0);  // start_frame
      put32(cmd, 0);  // number_of_packets
      put32(cmd, 1);  // interval
      for (int i = 0; i < 8; ++i) {
        cmd.push_back(setup ? setup[i] : 0);
      }
      cmd.insert(cmd.end(), out.begin(), out.end());
      net::send_all(socket_, cmd.data(), cmd.size());
      return seqnum;
    }

    void unlink(std::uint32_t victim) {
      std::vector<std::uint8_t> cmd;
      put32(cmd, usbip::proto::kCmdUnlink);
      put32(cmd, ++seqnum_);
      put32(cmd, 0x00010001);
      put32(cmd, 0);
      put32(cmd, 0);
      put32(cmd, victim);
      cmd.resize(usbip::proto::kUrbHeaderSize, 0);
      net::send_all(socket_, cmd.data(), cmd.size());
    }

    /** Read one reply; RET_UNLINK replies come back with command == 4 in `seqnum` high bit cleared. */
    std::optional<RetSubmit> read_reply(std::uint32_t *command = nullptr) {
      std::uint8_t header[usbip::proto::kUrbHeaderSize];
      if (!net::recv_all(socket_, header, sizeof(header))) {
        return std::nullopt;
      }
      RetSubmit ret;
      const auto cmd = be32(header);
      if (command) {
        *command = cmd;
      }
      ret.seqnum = be32(header + 4);
      ret.status = static_cast<std::int32_t>(be32(header + 20));
      if (cmd == usbip::proto::kRetSubmit) {
        ret.actual_length = static_cast<std::int32_t>(be32(header + 24));
        // IN data follows; OUT replies carry none. Tests track direction themselves.
        if (expect_in_data_ && ret.actual_length > 0) {
          ret.data.resize(static_cast<std::size_t>(ret.actual_length));
          if (!net::recv_all(socket_, ret.data.data(), ret.data.size())) {
            return std::nullopt;
          }
        }
      }
      return ret;
    }

    /** Control transfer, synchronously. */
    std::optional<RetSubmit> control(std::uint8_t request_type, std::uint8_t request, std::uint16_t value,
                                     std::uint16_t index, std::uint16_t length, const std::vector<std::uint8_t> &out = {}) {
      const std::uint8_t setup[8] = {request_type, request, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                     static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8),
                                     static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8)};
      const bool in = (request_type & 0x80) != 0;
      submit(in ? usbip::proto::kDirIn : usbip::proto::kDirOut, 0, in ? length : static_cast<std::uint32_t>(out.size()), setup, out);
      expect_in_data_ = in;
      return read_reply();
    }

    void expect_in_data(bool value) {
      expect_in_data_ = value;
    }

    void disconnect() {
      net::close(socket_);
      socket_ = net::kInvalidSocket;
    }

  private:
    static std::uint32_t be32(const std::uint8_t *p) {
      return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }

    static std::uint16_t be16(const std::uint8_t *p) {
      return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }

    static void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
      out.push_back(static_cast<std::uint8_t>(v >> 24));
      out.push_back(static_cast<std::uint8_t>(v >> 16));
      out.push_back(static_cast<std::uint8_t>(v >> 8));
      out.push_back(static_cast<std::uint8_t>(v));
    }

    static std::vector<std::uint8_t> op_header(std::uint16_t code) {
      return {0x01, 0x11, static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code), 0, 0, 0, 0};
    }

    std::uint16_t port_;
    net::Socket socket_ = net::kInvalidSocket;
    std::uint32_t seqnum_ = 0;
    bool expect_in_data_ = true;
  };

}  // namespace inputline::test
