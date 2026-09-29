/**
 * @file net.h
 * @brief Thin blocking-socket wrapper over Winsock and BSD sockets.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

namespace couchlink::net {

#ifdef _WIN32
  using Socket = SOCKET;
  inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
  using Socket = int;
  inline constexpr Socket kInvalidSocket = -1;
#endif

  /** Call once per process before any other function (Winsock needs it). */
  bool startup();

  void close(Socket socket);
  /** Unblock any thread waiting on the socket. */
  void shutdown(Socket socket);

  bool send_all(Socket socket, const void *data, std::size_t length);

  /**
   * @brief Read exactly @p length bytes.
   * @param stop If given, the read gives up soon after it becomes true. Windows
   *             does not wake a blocked recv() on shutdown(), so threads that
   *             must be stoppable pass a flag instead of relying on that.
   */
  bool recv_all(Socket socket, void *data, std::size_t length, const std::atomic<bool> *stop = nullptr);

  struct Endpoint {
    sockaddr_storage address {};
    socklen_t length = 0;

    std::string to_string() const;
    bool same_as(const Endpoint &other) const;
  };

  /** Resolve a numeric IPv4/IPv6 address or host name. */
  bool resolve(const std::string &host, std::uint16_t port, Endpoint &out);

  /**
   * @brief Listen for TCP connections.
   * @param bound_port Receives the actual port (useful with port 0).
   */
  Socket tcp_listen(const std::string &address, std::uint16_t port, std::uint16_t *bound_port = nullptr);
  Socket tcp_accept(Socket listener);
  Socket tcp_connect(const std::string &host, std::uint16_t port);
  void set_no_delay(Socket socket);

  Socket udp_bind(const std::string &address, std::uint16_t port, std::uint16_t *bound_port = nullptr);

  /**
   * @brief Receive one datagram.
   * @return Bytes received, 0 on timeout, -1 on error.
   */
  int udp_recv(Socket socket, void *buffer, std::size_t capacity, Endpoint &from, int timeout_ms);
  bool udp_send(Socket socket, const void *data, std::size_t length, const Endpoint &to);

}  // namespace couchlink::net
