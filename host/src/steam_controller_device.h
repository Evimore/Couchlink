/**
 * @file steam_controller_device.h
 * @brief A virtual wired 2026 Steam Controller (28DE:1302) for the USB/IP server.
 *
 * Steam sees the real device's USB identity and HID descriptor, gets its
 * claim-time feature queries answered locally, and receives state reports
 * converted from the Bluetooth frames the client forwards. Haptic output
 * reports and settings commands flow back out through the output callback.
 */
#pragma once

#include "couchlink/feature_responder.h"
#include "couchlink/link_protocol.h"
#include "couchlink/report_converter.h"
#include "usbip_server.h"

#include <bitset>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace couchlink {

  class SteamControllerDevice: public usbip::UsbDevice {
  public:
    using OutputCallback = std::function<void(link::OutputKind kind, const std::vector<std::uint8_t> &report)>;

    struct Stats {
      std::uint64_t reports_received = 0;
      std::uint64_t reports_delivered = 0;
      std::uint64_t reports_dropped = 0;
      std::uint64_t outputs_forwarded = 0;
      std::uint64_t features_blocked = 0;
    };

    explicit SteamControllerDevice(ControllerIdentity identity = default_identity());

    void set_output_callback(OutputCallback callback);

    /** Settings (SET_SETTINGS_VALUES IDs) never passed to the physical controller. */
    void set_blocked_settings(std::set<std::uint8_t> settings);

    /**
     * @brief Feed one raw input report from the physical controller.
     *
     * Accepts state reports 0x42/0x45/0x47 and battery report 0x43. Other
     * reports are ignored. Returns false when the report was not usable.
     */
    bool submit_report(const std::uint8_t *report, std::size_t length);

    /** Queue a neutral state (nothing pressed) so nothing sticks when the link drops. */
    void release_all();

    Stats stats() const;

    // UsbDevice
    std::vector<std::uint8_t> device_descriptor() const override;
    std::vector<std::uint8_t> configuration_descriptor() const override;
    usbip::ControlResult control(const usbip::SetupPacket &setup, const std::vector<std::uint8_t> &out_data) override;
    void interrupt_out(std::uint8_t endpoint, const std::vector<std::uint8_t> &data) override;
    bool pop_interrupt_in(std::uint8_t endpoint, std::vector<std::uint8_t> &report) override;

    static constexpr std::size_t kMaxQueuedReports = 8;
    static constexpr std::size_t kBatteryReportSize = 15;

  private:
    usbip::ControlResult standard_request(const usbip::SetupPacket &setup);
    usbip::ControlResult class_request(const usbip::SetupPacket &setup, const std::vector<std::uint8_t> &out_data);
    void queue_locked(std::vector<std::uint8_t> report);
    void emit_output(link::OutputKind kind, const std::vector<std::uint8_t> &report);
    /** Log lines for the parts of a forwarded command not logged before (call with mutex_ held). */
    std::vector<std::string> describe_new_forward(const std::vector<std::uint8_t> &report);

    mutable std::mutex mutex_;
    StateReportConverter converter_;
    FeatureResponder responder_;
    std::deque<std::vector<std::uint8_t>> queue_;
    StateReport last_state_ {};
    bool have_state_ = false;
    Stats stats_;
    std::bitset<256> blocked_logged_;  // log each filtered command once
    std::set<std::uint32_t> forwarded_logged_;  // command << 24 | setting << 16 | value, logged once
    std::set<std::uint8_t> blocked_settings_ {kSettingWirelessPacketVersion};

    std::mutex output_mutex_;
    OutputCallback output_;
  };

}  // namespace couchlink
