#include "usbip_server.h"

#include "log.h"

#include <algorithm>
#include <cstring>
#include <deque>

namespace inputline::usbip {

  using namespace proto;

  namespace {
    std::uint32_t get_u32be(const std::uint8_t *p) {
      return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    }

    std::string hex_bytes(const std::uint8_t *p, std::size_t n) {
      static constexpr char kDigits[] = "0123456789abcdef";
      std::string out;
      for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kDigits[p[i] >> 4]);
        out.push_back(kDigits[p[i] & 0x0F]);
      }
      return out;
    }

    std::uint16_t get_u16be(const std::uint8_t *p) {
      return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }

    void put_u32be(std::vector<std::uint8_t> &out, std::uint32_t v) {
      out.push_back(static_cast<std::uint8_t>(v >> 24));
      out.push_back(static_cast<std::uint8_t>(v >> 16));
      out.push_back(static_cast<std::uint8_t>(v >> 8));
      out.push_back(static_cast<std::uint8_t>(v));
    }

    void put_u16be(std::vector<std::uint8_t> &out, std::uint16_t v) {
      out.push_back(static_cast<std::uint8_t>(v >> 8));
      out.push_back(static_cast<std::uint8_t>(v));
    }

    void put_op_header(std::vector<std::uint8_t> &out, std::uint16_t code, std::uint32_t status) {
      put_u16be(out, kVersion);
      put_u16be(out, code);
      put_u32be(out, status);
    }

    struct InterfaceInfo {
      std::uint8_t cls = 0;
      std::uint8_t subclass = 0;
      std::uint8_t protocol = 0;
    };

    struct DeviceSummary {
      std::uint16_t vendor = 0;
      std::uint16_t product = 0;
      std::uint16_t bcd_device = 0;
      std::uint8_t cls = 0;
      std::uint8_t subclass = 0;
      std::uint8_t protocol = 0;
      std::uint8_t num_configurations = 1;
      std::uint8_t configuration_value = 1;
      std::vector<InterfaceInfo> interfaces;
    };

    DeviceSummary summarize(const UsbDevice &device) {
      DeviceSummary s;
      const auto dd = device.device_descriptor();
      if (dd.size() >= 18) {
        s.cls = dd[4];
        s.subclass = dd[5];
        s.protocol = dd[6];
        s.vendor = static_cast<std::uint16_t>(dd[8] | (dd[9] << 8));
        s.product = static_cast<std::uint16_t>(dd[10] | (dd[11] << 8));
        s.bcd_device = static_cast<std::uint16_t>(dd[12] | (dd[13] << 8));
        s.num_configurations = dd[17];
      }
      const auto cd = device.configuration_descriptor();
      if (cd.size() >= 9) {
        s.configuration_value = cd[5];
      }
      for (std::size_t pos = 0; pos + 2 <= cd.size() && cd[pos] > 0; pos += cd[pos]) {
        if (cd[pos + 1] == 4 && pos + 9 <= cd.size()) {
          s.interfaces.push_back({cd[pos + 5], cd[pos + 6], cd[pos + 7]});
        }
      }
      return s;
    }

    void put_usb_device(std::vector<std::uint8_t> &out, const std::string &busid, std::uint32_t devnum, const DeviceSummary &s) {
      char path[256] = {};
      std::snprintf(path, sizeof(path), "/sys/devices/platform/inputline/usb1/%s", busid.c_str());
      out.insert(out.end(), path, path + sizeof(path));
      char bus[32] = {};
      std::snprintf(bus, sizeof(bus), "%s", busid.c_str());
      out.insert(out.end(), bus, bus + sizeof(bus));
      put_u32be(out, 1);  // busnum
      put_u32be(out, devnum);
      put_u32be(out, kSpeedFull);
      put_u16be(out, s.vendor);
      put_u16be(out, s.product);
      put_u16be(out, s.bcd_device);
      out.push_back(s.cls);
      out.push_back(s.subclass);
      out.push_back(s.protocol);
      out.push_back(s.configuration_value);
      out.push_back(s.num_configurations);
      out.push_back(static_cast<std::uint8_t>(s.interfaces.size()));
    }
  }  // namespace

  // ---- UsbDevice ------------------------------------------------------------

  void UsbDevice::set_in_ready_callback(std::function<void()> callback) {
    std::lock_guard lock(callback_mutex_);
    in_ready_ = std::move(callback);
  }

  void UsbDevice::notify_in_ready() {
    std::function<void()> callback;
    {
      std::lock_guard lock(callback_mutex_);
      callback = in_ready_;
    }
    if (callback) {
      callback();
    }
  }

  // ---- Connection -----------------------------------------------------------

  /** Linux's limit for one URB (USBIP_MAX_ISO_PACKETS). */
  constexpr std::int32_t kMaxIsoPackets = 1024;

  class Connection {
  public:
    Connection(net::Socket socket, std::shared_ptr<UsbDevice> device, std::string busid):
        socket_(socket),
        device_(std::move(device)),
        busid_(std::move(busid)) {}

    /** Serve URBs until the peer disconnects or close() is called. */
    void run() {
      std::uint8_t header[kUrbHeaderSize];
      while (net::recv_all(socket_, header, sizeof(header), &closing_)) {
        const auto command = get_u32be(header);
        bool ok = false;
        if (command == kCmdSubmit) {
          ok = handle_submit(header);
        } else if (command == kCmdUnlink) {
          ok = handle_unlink(header);
        } else {
          log::warn("usbip ", busid_, ": unknown command ", command, " header ", hex_bytes(header, sizeof(header)));
        }
        if (!ok) {
          break;
        }
      }
      std::lock_guard lock(urb_mutex_);
      pending_in_.clear();
    }

    void close() {
      closing_ = true;
      net::shutdown(socket_);
    }

    /** Complete pending interrupt IN URBs with queued reports. */
    void pump() {
      std::lock_guard lock(urb_mutex_);
      while (!pending_in_.empty()) {
        const auto urb = pending_in_.front();
        std::vector<std::uint8_t> report;
        if (!device_->pop_interrupt_in(urb.endpoint, report)) {
          break;
        }
        pending_in_.pop_front();
        if (report.size() > urb.length) {
          report.resize(urb.length);
        }
        send_ret_submit(urb.seqnum, 0, report.data(), report.size(), report.size());
      }
    }

  private:
    struct PendingIn {
      std::uint32_t seqnum;
      std::uint8_t endpoint;
      std::uint32_t length;
    };

    bool handle_submit(const std::uint8_t *h) {
      const auto seqnum = get_u32be(h + 4);
      const auto direction = get_u32be(h + 12);
      const auto endpoint = static_cast<std::uint8_t>(get_u32be(h + 16));
      const auto buffer_length = static_cast<std::int32_t>(get_u32be(h + 24));
      const auto packets = static_cast<std::int32_t>(get_u32be(h + 32));
      if (endpoint != 0 && log::enabled(log::Level::kDebug) && debug_budget_ > 0) {
        --debug_budget_;
        log::debug("usbip ", busid_, ": seq ", seqnum, " ep ", int(endpoint), direction == kDirIn ? " in" : " out", " len ", buffer_length, " packets ", packets);
      }
      const std::uint8_t *setup_bytes = h + 40;

      if (buffer_length < 0 || buffer_length > 65536) {
        return false;
      }

      std::vector<std::uint8_t> out_data;
      if (direction == kDirOut && buffer_length > 0) {
        out_data.resize(static_cast<std::size_t>(buffer_length));
        if (!net::recv_all(socket_, out_data.data(), out_data.size(), &closing_)) {
          return false;
        }
      }
      if (packets > kMaxIsoPackets) {
        return false;  // no real client sends that many; don't allocate for it
      }
      if (packets > 0) {
        // Isochronous transfers: this server exposes no isochronous endpoints.
        std::vector<std::uint8_t> descriptors(static_cast<std::size_t>(packets) * 16);
        if (!net::recv_all(socket_, descriptors.data(), descriptors.size(), &closing_)) {
          return false;
        }
        return send_ret_submit(seqnum, kStatusStall, nullptr, 0, 0);
      }

      if (endpoint == 0) {
        log::debug("usbip ", busid_, ": seq ", seqnum, " ctrl ", direction == kDirIn ? "in " : "out ", hex_bytes(setup_bytes, 8), " len ", buffer_length);
        SetupPacket setup;
        setup.request_type = setup_bytes[0];
        setup.request = setup_bytes[1];
        setup.value = static_cast<std::uint16_t>(setup_bytes[2] | (setup_bytes[3] << 8));
        setup.index = static_cast<std::uint16_t>(setup_bytes[4] | (setup_bytes[5] << 8));
        setup.length = static_cast<std::uint16_t>(setup_bytes[6] | (setup_bytes[7] << 8));

        auto result = device_->control(setup, out_data);
        if (result.stall) {
          log::debug("usbip ", busid_, ": stall request_type=0x", std::hex, int(setup.request_type), " request=0x", int(setup.request), " value=0x", setup.value);
          return send_ret_submit(seqnum, kStatusStall, nullptr, 0, 0);
        }
        if (direction == kDirIn) {
          const auto limit = std::min<std::size_t>(setup.length, static_cast<std::size_t>(buffer_length));
          if (result.data.size() > limit) {
            result.data.resize(limit);
          }
          log::debug("usbip ", busid_, ": seq ", seqnum, " -> ", result.data.size(), " bytes");
          return send_ret_submit(seqnum, 0, result.data.data(), result.data.size(), result.data.size());
        }
        log::debug("usbip ", busid_, ": seq ", seqnum, " -> ok");
        return send_ret_submit(seqnum, 0, nullptr, 0, out_data.size());
      }

      if (direction == kDirIn) {
        {
          std::lock_guard lock(urb_mutex_);
          pending_in_.push_back({seqnum, endpoint, static_cast<std::uint32_t>(buffer_length)});
        }
        pump();
        return true;
      }

      device_->interrupt_out(endpoint, out_data);
      return send_ret_submit(seqnum, 0, nullptr, 0, out_data.size());
    }

    bool handle_unlink(const std::uint8_t *h) {
      const auto seqnum = get_u32be(h + 4);
      const auto victim = get_u32be(h + 20);
      log::debug("usbip ", busid_, ": unlink seq ", victim);
      std::int32_t status = 0;
      {
        std::lock_guard lock(urb_mutex_);
        const auto it = std::find_if(pending_in_.begin(), pending_in_.end(), [victim](const PendingIn &urb) {
          return urb.seqnum == victim;
        });
        if (it != pending_in_.end()) {
          pending_in_.erase(it);
          status = kStatusUnlinked;
        }
      }

      std::vector<std::uint8_t> reply;
      reply.reserve(kUrbHeaderSize);
      put_u32be(reply, kRetUnlink);
      put_u32be(reply, seqnum);
      put_u32be(reply, 0);
      put_u32be(reply, 0);
      put_u32be(reply, 0);
      put_u32be(reply, static_cast<std::uint32_t>(status));
      reply.resize(kUrbHeaderSize, 0);
      std::lock_guard lock(write_mutex_);
      return net::send_all(socket_, reply.data(), reply.size());
    }

    bool send_ret_submit(std::uint32_t seqnum, std::int32_t status, const std::uint8_t *data, std::size_t data_length, std::size_t actual_length) {
      std::vector<std::uint8_t> reply;
      reply.reserve(kUrbHeaderSize + data_length);
      put_u32be(reply, kRetSubmit);
      put_u32be(reply, seqnum);
      put_u32be(reply, 0);  // devid
      put_u32be(reply, 0);  // direction
      put_u32be(reply, 0);  // ep
      put_u32be(reply, static_cast<std::uint32_t>(status));
      put_u32be(reply, static_cast<std::uint32_t>(actual_length));
      put_u32be(reply, 0);  // start_frame
      put_u32be(reply, 0);  // number_of_packets
      put_u32be(reply, 0);  // error_count
      reply.resize(kUrbHeaderSize, 0);
      if (data_length > 0) {
        reply.insert(reply.end(), data, data + data_length);
      }
      std::lock_guard lock(write_mutex_);
      return net::send_all(socket_, reply.data(), reply.size());
    }

    net::Socket socket_;
    std::shared_ptr<UsbDevice> device_;
    std::string busid_;
    std::atomic<bool> closing_ {false};
    std::mutex write_mutex_;
    std::mutex urb_mutex_;
    std::deque<PendingIn> pending_in_;
    int debug_budget_ = 16;  // interrupt URBs to log at debug level
  };

  // ---- Server ---------------------------------------------------------------

  Server::Server() = default;

  Server::~Server() {
    stop();
  }

  bool Server::start(const std::string &address, std::uint16_t port) {
    listener_ = net::tcp_listen(address, port, &port_);
    if (listener_ == net::kInvalidSocket) {
      log::error("usbip: cannot listen on ", address, ":", port);
      return false;
    }
    running_ = true;
    stopping_ = false;
    accept_thread_ = std::thread(&Server::accept_loop, this, listener_);
    log::info("usbip: serving on ", address, ":", port_);
    return true;
  }

  void Server::stop() {
    if (!running_.exchange(false)) {
      return;
    }
    stopping_ = true;
    {
      std::lock_guard lock(mutex_);
      for (auto &[busid, entry] : exports_) {
        if (entry.connection) {
          entry.connection->close();
        }
      }
    }
    // Linux needs shutdown() to wake a blocked accept(); Windows needs closesocket().
    net::shutdown(listener_);
    net::close(listener_);
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    listener_ = net::kInvalidSocket;

    std::unique_lock lock(clients_mutex_);
    for (auto socket : client_sockets_) {
      net::shutdown(socket);
    }
    clients_idle_.wait(lock, [this] {
      return client_sockets_.empty();
    });
  }

  void Server::accept_loop(net::Socket listener) {
    while (running_) {
      const auto client = net::tcp_accept(listener);
      if (client == net::kInvalidSocket) {
        if (!running_) {
          break;
        }
        continue;
      }
      if (!running_) {
        net::close(client);
        break;
      }
      net::set_no_delay(client);
      {
        std::lock_guard lock(clients_mutex_);
        client_sockets_.push_back(client);
      }
      std::thread([this, client] {
        handle_client(client);
        std::lock_guard lock(clients_mutex_);
        client_sockets_.erase(std::remove(client_sockets_.begin(), client_sockets_.end(), client), client_sockets_.end());
        net::close(client);
        clients_idle_.notify_all();
      }).detach();
    }
  }

  void Server::handle_client(net::Socket socket) {
    std::uint8_t header[8];
    if (!net::recv_all(socket, header, sizeof(header), &stopping_)) {
      return;
    }
    const auto code = get_u16be(header + 2);
    if (code == kOpReqDevlist) {
      reply_devlist(socket);
    } else if (code == kOpReqImport) {
      reply_import(socket);
    } else {
      log::warn("usbip: unknown operation 0x", std::hex, code);
    }
  }

  void Server::reply_devlist(net::Socket socket) {
    std::vector<std::uint8_t> reply;
    put_op_header(reply, kOpRepDevlist, 0);
    std::lock_guard lock(mutex_);
    std::uint32_t count = 0;
    for (const auto &[busid, entry] : exports_) {
      count += entry.connection ? 0 : 1;
    }
    put_u32be(reply, count);
    for (const auto &[busid, entry] : exports_) {
      if (entry.connection) {
        continue;
      }
      const auto summary = summarize(*entry.device);
      put_usb_device(reply, busid, entry.devnum, summary);
      for (const auto &interface : summary.interfaces) {
        reply.push_back(interface.cls);
        reply.push_back(interface.subclass);
        reply.push_back(interface.protocol);
        reply.push_back(0);
      }
    }
    net::send_all(socket, reply.data(), reply.size());
  }

  void Server::reply_import(net::Socket socket) {
    char busid_raw[33] = {};
    if (!net::recv_all(socket, busid_raw, 32, &stopping_)) {
      return;
    }
    const std::string busid(busid_raw);

    std::shared_ptr<Connection> connection;
    std::shared_ptr<UsbDevice> device;
    std::vector<std::uint8_t> reply;
    {
      std::lock_guard lock(mutex_);
      const auto it = exports_.find(busid);
      if (it == exports_.end() || it->second.connection) {
        put_op_header(reply, kOpRepImport, 1);
        net::send_all(socket, reply.data(), reply.size());
        // usbip-win2 keeps retrying devices it attached before; harmless.
        log::debug("usbip: import of ", busid, " refused (unknown or busy)");
        return;
      }
      device = it->second.device;
      connection = std::make_shared<Connection>(socket, device, busid);
      it->second.connection = connection;
      put_op_header(reply, kOpRepImport, 0);
      put_usb_device(reply, busid, it->second.devnum, summarize(*device));
    }

    std::weak_ptr<Connection> weak = connection;
    device->set_in_ready_callback([weak] {
      if (auto strong = weak.lock()) {
        strong->pump();
      }
    });

    if (!net::send_all(socket, reply.data(), reply.size())) {
      device->set_in_ready_callback({});
      std::lock_guard lock(mutex_);
      const auto it = exports_.find(busid);
      if (it != exports_.end() && it->second.connection == connection) {
        it->second.connection.reset();
      }
      return;
    }

    log::info("usbip: ", busid, " attached");
    std::function<void(const std::string &, bool)> callback;
    {
      std::lock_guard lock(mutex_);
      callback = attach_callback_;
    }
    if (callback) {
      callback(busid, true);
    }

    connection->run();

    device->set_in_ready_callback({});
    {
      std::lock_guard lock(mutex_);
      const auto it = exports_.find(busid);
      if (it != exports_.end() && it->second.connection == connection) {
        it->second.connection.reset();
      }
      callback = attach_callback_;
    }
    log::info("usbip: ", busid, " detached");
    if (callback) {
      callback(busid, false);
    }
  }

  std::string Server::add_device(std::shared_ptr<UsbDevice> device) {
    std::lock_guard lock(mutex_);
    // Reuse the lowest free slot so a replugged controller keeps its bus ID.
    std::uint32_t devnum = 1;
    while (exports_.count("1-" + std::to_string(devnum)) != 0) {
      ++devnum;
    }
    const auto busid = "1-" + std::to_string(devnum);
    exports_[busid] = Export {devnum, std::move(device), nullptr};
    return busid;
  }

  void Server::remove_device(const std::string &busid) {
    std::shared_ptr<Connection> connection;
    {
      std::lock_guard lock(mutex_);
      const auto it = exports_.find(busid);
      if (it == exports_.end()) {
        return;
      }
      connection = it->second.connection;
      exports_.erase(it);
    }
    if (connection) {
      connection->close();
    }
  }

  bool Server::is_attached(const std::string &busid) const {
    std::lock_guard lock(mutex_);
    const auto it = exports_.find(busid);
    return it != exports_.end() && it->second.connection != nullptr;
  }

  void Server::set_attach_callback(std::function<void(const std::string &, bool)> callback) {
    std::lock_guard lock(mutex_);
    attach_callback_ = std::move(callback);
  }

}  // namespace inputline::usbip
