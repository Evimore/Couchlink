/**
 * @file discovery.h
 * @brief Announce couchlink-host on the local network, so the Couchlink app
 *        can list the PC instead of asking for its address.
 *
 * The PC is registered as a DNS-SD service of type `_couchlink._udp` through
 * the operating system's own mDNS responder (Windows 10 1809 and later).
 * Elsewhere, start() returns false and clients enter the address by hand.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace couchlink::discovery {

  /** DNS-SD service type, as the app browses for it. */
  inline constexpr const char *kServiceType = "_couchlink._udp";

  /**
   * The DNS-SD instance name for a PC name: one DNS label, so dots become
   * dashes, and at most 63 bytes (cut at a UTF-8 character boundary).
   */
  std::string instance_label(const std::string &name);

  class Advertiser {
  public:
    Advertiser();
    ~Advertiser();
    Advertiser(const Advertiser &) = delete;
    Advertiser &operator=(const Advertiser &) = delete;

    /** Start announcing `name` on UDP `port`. False where DNS-SD is unavailable. */
    bool start(const std::string &name, std::uint16_t port);
    void stop();

  private:
    struct Impl;
    Impl *impl_ = nullptr;  // may outlive this object if Windows never confirms the deregistration
  };

}  // namespace couchlink::discovery
