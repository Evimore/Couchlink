/**
 * @file usbip_server.h
 * @brief A small USB/IP device server for emulated USB devices.
 *
 * USB/IP (https://docs.kernel.org/usb/usbip_protocol.html) carries USB
 * requests over TCP. The operating system's USB/IP client attaches to this
 * server over loopback and sees each exported device as real USB hardware:
 * on Windows through usbip-win2 (Microsoft-signed driver), on Linux through
 * the in-kernel vhci-hcd. No custom driver is involved.
 *
 * Supports control and interrupt transfers, which is all a HID device needs.
 */
#pragma once

#include "net.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace couchlink::usbip {

  constexpr std::uint16_t kDefaultPort = 3240;

  struct SetupPacket {
    std::uint8_t request_type = 0;
    std::uint8_t request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
    std::uint16_t length = 0;
  };

  struct ControlResult {
    bool stall = false;
    std::vector<std::uint8_t> data;  ///< Data stage for device-to-host requests.

    static ControlResult ok(std::vector<std::uint8_t> data = {}) {
      return {false, std::move(data)};
    }

    static ControlResult stalled() {
      return {true, {}};
    }
  };

  /** An emulated USB device. Implementations must be thread-safe. */
  class UsbDevice {
  public:
    virtual ~UsbDevice() = default;

    virtual std::vector<std::uint8_t> device_descriptor() const = 0;
    virtual std::vector<std::uint8_t> configuration_descriptor() const = 0;

    /** Handle a request on endpoint 0 (standard, class or vendor). */
    virtual ControlResult control(const SetupPacket &setup, const std::vector<std::uint8_t> &out_data) = 0;

    /** Data the host wrote to an interrupt OUT endpoint. */
    virtual void interrupt_out(std::uint8_t endpoint, const std::vector<std::uint8_t> &data) = 0;

    /** Take the next queued interrupt IN report for @p endpoint, if any. */
    virtual bool pop_interrupt_in(std::uint8_t endpoint, std::vector<std::uint8_t> &report) = 0;

    /** The server registers this to learn when new IN data is queued. */
    void set_in_ready_callback(std::function<void()> callback);

  protected:
    void notify_in_ready();

  private:
    std::mutex callback_mutex_;
    std::function<void()> in_ready_;
  };

  class Connection;

  class Server {
  public:
    Server();
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    /** Start listening. Use port 0 to pick a free port (tests). */
    bool start(const std::string &address, std::uint16_t port);
    void stop();
    std::uint16_t port() const {
      return port_;
    }

    /** Export a device; returns its bus ID ("1-1", "1-2", ...), reusing freed slots. */
    std::string add_device(std::shared_ptr<UsbDevice> device);

    /** Stop exporting a device and unplug it if attached. */
    void remove_device(const std::string &busid);

    bool is_attached(const std::string &busid) const;

    /** Called when a device gets attached or detached by the OS. */
    void set_attach_callback(std::function<void(const std::string &busid, bool attached)> callback);

  private:
    struct Export {
      std::uint32_t devnum = 0;
      std::shared_ptr<UsbDevice> device;
      std::shared_ptr<Connection> connection;
    };

    void accept_loop(net::Socket listener);
    void handle_client(net::Socket socket);
    void reply_devlist(net::Socket socket);
    void reply_import(net::Socket socket);

    mutable std::mutex mutex_;
    std::map<std::string, Export> exports_;
    std::function<void(const std::string &, bool)> attach_callback_;

    net::Socket listener_ = net::kInvalidSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_ {false};
    std::atomic<bool> stopping_ {false};  ///< Wakes client threads blocked in reads.
    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::condition_variable clients_idle_;
    std::vector<net::Socket> client_sockets_;  ///< Open client sockets, each served by a detached thread.
  };

  // Protocol constants, exposed for tests.
  namespace proto {
    constexpr std::uint16_t kVersion = 0x0111;
    constexpr std::uint16_t kOpReqDevlist = 0x8005;
    constexpr std::uint16_t kOpRepDevlist = 0x0005;
    constexpr std::uint16_t kOpReqImport = 0x8003;
    constexpr std::uint16_t kOpRepImport = 0x0003;
    constexpr std::uint32_t kCmdSubmit = 1;
    constexpr std::uint32_t kCmdUnlink = 2;
    constexpr std::uint32_t kRetSubmit = 3;
    constexpr std::uint32_t kRetUnlink = 4;
    constexpr std::uint32_t kDirOut = 0;
    constexpr std::uint32_t kDirIn = 1;
    constexpr std::size_t kUrbHeaderSize = 48;
    constexpr std::size_t kUsbDeviceSize = 312;
    constexpr std::int32_t kStatusStall = -32;  // -EPIPE
    constexpr std::int32_t kStatusUnlinked = -104;  // -ECONNRESET
    constexpr std::uint32_t kSpeedFull = 2;
  }  // namespace proto

}  // namespace couchlink::usbip
