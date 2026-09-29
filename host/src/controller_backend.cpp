#include "controller_backend.h"

#include "log.h"
#include "steam_controller_device.h"
#include "usbip_server.h"

#include <algorithm>
#include <thread>

namespace couchlink {

  namespace {
    class UsbipController: public VirtualController {
    public:
      UsbipController(usbip::Server &server, std::shared_ptr<SteamControllerDevice> device, std::string busid):
          server_(server),
          device_(std::move(device)),
          busid_(std::move(busid)) {}

      ~UsbipController() override {
        device_->set_output_callback({});
        server_.remove_device(busid_);
        log::info("controller ", busid_, " removed");
      }

      bool submit(const std::uint8_t *report, std::size_t length) override {
        return device_->submit_report(report, length);
      }

      void release_all() override {
        device_->release_all();
      }

    private:
      usbip::Server &server_;
      std::shared_ptr<SteamControllerDevice> device_;
      std::string busid_;
    };

    ControllerIdentity identity_for(const link::Attach &attach, std::uint8_t instance, bool use_client_attributes) {
      auto identity = default_identity(instance);

      const auto serial_end = std::find(attach.unit_serial.begin(), attach.unit_serial.end(), '\0');
      std::string serial(attach.unit_serial.begin(), serial_end);
      const bool printable = !serial.empty() && std::all_of(serial.begin(), serial.end(), [](char c) {
        return c > 0x20 && c < 0x7F;
      });
      if (printable) {
        // Steam keys per-controller settings by serial, so pass the real one through.
        identity.unit_serial = serial;
      }

      if (use_client_attributes && attach.attributes_reply[1] == kCmdGetAttributesValues) {
        identity.attributes_reply = attach.attributes_reply;
      } else if (adopt_firmware_attributes(identity, attach.attributes_reply)) {
        log::debug("controller: reporting the physical controller's firmware version");
      }
      return identity;
    }
  }  // namespace

  UsbipBackend::UsbipBackend(usbip::Server &server, UsbipBackendOptions options):
      server_(server),
      options_(std::move(options)) {}

  std::unique_ptr<VirtualController> UsbipBackend::create(const link::Attach &attach, std::uint8_t instance, OutputSink sink) {
    if (attach.kind != link::DeviceKind::kSteamController2026) {
      return nullptr;
    }

    auto device = std::make_shared<SteamControllerDevice>(identity_for(attach, instance, options_.use_client_attributes));
    device->set_output_callback(std::move(sink));
    device->set_blocked_settings(options_.blocked_settings);
    const auto busid = server_.add_device(device);
    log::info("controller ", busid, " exported (Steam Controller 2026)");

    if (options_.attach.enabled) {
      // `usbip attach` connects back to our own server, so it must not block
      // the thread that serves the link.
      std::thread([options = options_.attach, busid] {
        if (run_usbip_attach(options, busid)) {
          log::info("controller ", busid, " attach requested");
        }
      }).detach();
    } else {
      log::info("controller ", busid, " ready: attach it with 'usbip attach -r 127.0.0.1 -b ", busid, "'");
    }

    return std::make_unique<UsbipController>(server_, device, busid);
  }

}  // namespace couchlink
