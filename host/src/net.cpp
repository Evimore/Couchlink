#include "net.h"

#include <cstring>

#ifndef _WIN32
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <unistd.h>
#endif

namespace inputline::net {

  namespace {
#ifdef _WIN32
    using IoLength = int;
#else
    using IoLength = std::size_t;
#endif

    bool resolve_passive(const std::string &address, std::uint16_t port, int type, addrinfo **result) {
      addrinfo hints {};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = type;
      hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
      const auto service = std::to_string(port);
      return getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(), &hints, result) == 0;
    }

    std::uint16_t local_port(Socket socket) {
      sockaddr_storage address {};
      socklen_t length = sizeof(address);
      if (getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return 0;
      }
      if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
      }
      return ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
    }

    Socket bind_socket(const std::string &address, std::uint16_t port, int type, std::uint16_t *bound_port) {
      addrinfo *info = nullptr;
      if (!resolve_passive(address, port, type, &info)) {
        return kInvalidSocket;
      }

      Socket result = kInvalidSocket;
      for (auto *entry = info; entry != nullptr; entry = entry->ai_next) {
        Socket candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (candidate == kInvalidSocket) {
          continue;
        }
        if (type == SOCK_STREAM) {
          int reuse = 1;
          setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        }
        if (entry->ai_family == AF_INET6) {
          // Accept IPv4 clients on an IPv6 socket too ("::" means every address).
          int v6_only = 0;
          setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6_only), sizeof(v6_only));
        }
        if (::bind(candidate, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0) {
          result = candidate;
          break;
        }
        close(candidate);
      }
      freeaddrinfo(info);

      if (result != kInvalidSocket && bound_port != nullptr) {
        *bound_port = local_port(result);
      }
      return result;
    }
  }  // namespace

  bool startup() {
#ifdef _WIN32
    WSADATA data {};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
  }

  void close(Socket socket) {
    if (socket == kInvalidSocket) {
      return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
  }

  void shutdown(Socket socket) {
    if (socket == kInvalidSocket) {
      return;
    }
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
  }

  bool send_all(Socket socket, const void *data, std::size_t length) {
    const auto *p = static_cast<const char *>(data);
    while (length > 0) {
#ifdef _WIN32
      const int sent = ::send(socket, p, static_cast<int>(length), 0);
#else
      const auto sent = ::send(socket, p, length, MSG_NOSIGNAL);
#endif
      if (sent <= 0) {
        return false;
      }
      p += sent;
      length -= static_cast<std::size_t>(sent);
    }
    return true;
  }

  namespace {
    /** Wait until readable: 1 ready, 0 timeout, -1 error. */
    int wait_readable(Socket socket, int timeout_ms) {
#ifdef _WIN32
      WSAPOLLFD pfd {};
      pfd.fd = socket;
      pfd.events = POLLRDNORM;
      return WSAPoll(&pfd, 1, timeout_ms);
#else
      pollfd pfd {};
      pfd.fd = socket;
      pfd.events = POLLIN;
      return ::poll(&pfd, 1, timeout_ms);
#endif
    }
  }  // namespace

  bool recv_all(Socket socket, void *data, std::size_t length, const std::atomic<bool> *stop) {
    auto *p = static_cast<char *>(data);
    while (length > 0) {
      if (stop != nullptr) {
        int ready = 0;
        while (ready == 0) {
          if (stop->load()) {
            return false;
          }
          ready = wait_readable(socket, 100);
        }
        if (ready < 0) {
          return false;
        }
      }
      const auto received = ::recv(socket, p, static_cast<IoLength>(length), 0);
      if (received <= 0) {
        return false;
      }
      p += received;
      length -= static_cast<std::size_t>(received);
    }
    return true;
  }

  std::string Endpoint::to_string() const {
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr *>(&address), length, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
      return "?";
    }
    if (address.ss_family == AF_INET6) {
      return std::string("[") + host + "]:" + service;
    }
    return std::string(host) + ":" + service;
  }

  bool Endpoint::same_as(const Endpoint &other) const {
    return length == other.length && std::memcmp(&address, &other.address, static_cast<std::size_t>(length)) == 0;
  }

  bool resolve(const std::string &host, std::uint16_t port, Endpoint &out) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *info = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &info) != 0 || info == nullptr) {
      return false;
    }
    std::memcpy(&out.address, info->ai_addr, info->ai_addrlen);
    out.length = static_cast<socklen_t>(info->ai_addrlen);
    freeaddrinfo(info);
    return true;
  }

  Socket tcp_listen(const std::string &address, std::uint16_t port, std::uint16_t *bound_port) {
    Socket socket = bind_socket(address, port, SOCK_STREAM, bound_port);
    if (socket != kInvalidSocket && ::listen(socket, 8) != 0) {
      close(socket);
      return kInvalidSocket;
    }
    return socket;
  }

  Socket tcp_accept(Socket listener) {
    return ::accept(listener, nullptr, nullptr);
  }

  Socket tcp_connect(const std::string &host, std::uint16_t port) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *info = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &info) != 0) {
      return kInvalidSocket;
    }
    Socket result = kInvalidSocket;
    for (auto *entry = info; entry != nullptr; entry = entry->ai_next) {
      Socket candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
      if (candidate == kInvalidSocket) {
        continue;
      }
      if (::connect(candidate, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0) {
        result = candidate;
        break;
      }
      close(candidate);
    }
    freeaddrinfo(info);
    return result;
  }

  void set_no_delay(Socket socket) {
    int enable = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&enable), sizeof(enable));
  }

  Socket udp_bind(const std::string &address, std::uint16_t port, std::uint16_t *bound_port) {
    return bind_socket(address, port, SOCK_DGRAM, bound_port);
  }

  int udp_recv(Socket socket, void *buffer, std::size_t capacity, Endpoint &from, int timeout_ms) {
    const int ready = wait_readable(socket, timeout_ms);
    if (ready == 0) {
      return 0;
    }
    if (ready < 0) {
      return -1;
    }

    from.length = sizeof(from.address);
    const auto received = ::recvfrom(socket, static_cast<char *>(buffer), static_cast<IoLength>(capacity), 0, reinterpret_cast<sockaddr *>(&from.address), &from.length);
    if (received < 0) {
#ifdef _WIN32
      // A previous send to a closed port surfaces here as WSAECONNRESET; ignore it.
      if (WSAGetLastError() == WSAECONNRESET) {
        return 0;
      }
#endif
      return -1;
    }
    return static_cast<int>(received);
  }

  bool udp_send(Socket socket, const void *data, std::size_t length, const Endpoint &to) {
    const auto sent = ::sendto(socket, static_cast<const char *>(data), static_cast<IoLength>(length), 0, reinterpret_cast<const sockaddr *>(&to.address), to.length);
    return sent >= 0 && static_cast<std::size_t>(sent) == length;
  }

}  // namespace inputline::net
