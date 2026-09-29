/**
 * @file controller_backend.h
 * @brief Where virtual controllers come from.
 *
 * The link server only needs "create a controller, feed it reports, drop it".
 * The production backend exports a USB/IP device and asks the OS to attach
 * it; tests substitute an in-memory backend.
 */
#pragma once

#include "inputline/feature_responder.h"
#include "inputline/link_protocol.h"
#include "usbip_attach.h"

#include <functional>
#include <memory>
#include <set>

namespace inputline {

  namespace usbip {
    class Server;
  }

  using OutputSink = std::function<void(link::OutputKind kind, const std::vector<std::uint8_t> &report)>;

  class VirtualController {
  public:
    virtual ~VirtualController() = default;

    /** Feed one raw report from the physical controller. */
    virtual bool submit(const std::uint8_t *report, std::size_t length) = 0;

    /** Release every button and centre every axis. */
    virtual void release_all() = 0;
  };

  class ControllerBackend {
  public:
    virtual ~ControllerBackend() = default;

    /**
     * @brief Create a virtual controller for a client's attach request.
     * @param instance Small number that keeps serials distinct across controllers.
     * @param sink Receives haptics and settings Steam sends to the controller.
     * @return nullptr if the device cannot be created.
     */
    virtual std::unique_ptr<VirtualController> create(const link::Attach &attach, std::uint8_t instance, OutputSink sink) = 0;
  };

  struct UsbipBackendOptions {
    AttachOptions attach;
    /** Report the client's real controller attributes instead of known-good defaults. */
    bool use_client_attributes = false;
    /** SET_SETTINGS_VALUES IDs never passed to the physical controller. */
    std::set<std::uint8_t> blocked_settings {kSettingWirelessPacketVersion};
  };

  /** Virtual USB Steam Controllers served over USB/IP. */
  class UsbipBackend: public ControllerBackend {
  public:
    UsbipBackend(usbip::Server &server, UsbipBackendOptions options);

    std::unique_ptr<VirtualController> create(const link::Attach &attach, std::uint8_t instance, OutputSink sink) override;

  private:
    usbip::Server &server_;
    UsbipBackendOptions options_;
  };

}  // namespace inputline
