#include "steam_controller_device.h"

#include "log.h"
#include "triton_descriptors.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace inputline {

  namespace {
    // bmRequestType values
    constexpr std::uint8_t kDeviceToHost = 0x80;
    constexpr std::uint8_t kTypeMask = 0x60;
    constexpr std::uint8_t kTypeStandard = 0x00;
    constexpr std::uint8_t kTypeClass = 0x20;
    constexpr std::uint8_t kRecipientMask = 0x1F;
    constexpr std::uint8_t kRecipientInterface = 0x01;

    // Standard requests
    constexpr std::uint8_t kGetStatus = 0x00;
    constexpr std::uint8_t kClearFeature = 0x01;
    constexpr std::uint8_t kSetFeature = 0x03;
    constexpr std::uint8_t kGetDescriptor = 0x06;
    constexpr std::uint8_t kGetConfiguration = 0x08;
    constexpr std::uint8_t kSetConfiguration = 0x09;
    constexpr std::uint8_t kGetInterface = 0x0A;
    constexpr std::uint8_t kSetInterface = 0x0B;

    // Descriptor types
    constexpr std::uint8_t kDescDevice = 0x01;
    constexpr std::uint8_t kDescConfiguration = 0x02;
    constexpr std::uint8_t kDescString = 0x03;
    constexpr std::uint8_t kDescHid = 0x21;
    constexpr std::uint8_t kDescHidReport = 0x22;

    // HID class requests
    constexpr std::uint8_t kHidGetReport = 0x01;
    constexpr std::uint8_t kHidGetIdle = 0x02;
    constexpr std::uint8_t kHidGetProtocol = 0x03;
    constexpr std::uint8_t kHidSetReport = 0x09;
    constexpr std::uint8_t kHidSetIdle = 0x0A;
    constexpr std::uint8_t kHidSetProtocol = 0x0B;

    constexpr std::uint8_t kReportTypeInput = 1;
    constexpr std::uint8_t kReportTypeOutput = 2;
    constexpr std::uint8_t kReportTypeFeature = 3;

    constexpr std::uint8_t kInterruptInEndpoint = 1;
    constexpr std::uint8_t kInterruptOutEndpoint = 1;

    std::vector<std::uint8_t> string_descriptor(const char *text) {
      std::vector<std::uint8_t> out {0, kDescString};
      for (const char *p = text; *p != '\0'; ++p) {
        out.push_back(static_cast<std::uint8_t>(*p));
        out.push_back(0);
      }
      out[0] = static_cast<std::uint8_t>(out.size());
      return out;
    }

    /** Make sure a report buffer starts with its report ID. */
    std::vector<std::uint8_t> with_report_id(std::uint8_t id, const std::vector<std::uint8_t> &data) {
      if (!data.empty() && data[0] == id) {
        return data;
      }
      std::vector<std::uint8_t> out;
      out.reserve(data.size() + 1);
      out.push_back(id);
      out.insert(out.end(), data.begin(), data.end());
      return out;
    }

    bool is_output_report(std::uint8_t id) {
      return id >= kOutputHapticRumble && id <= kOutputReportLast;
    }
  }  // namespace

  SteamControllerDevice::SteamControllerDevice(ControllerIdentity identity):
      responder_(std::move(identity)) {}

  void SteamControllerDevice::set_output_callback(OutputCallback callback) {
    std::lock_guard lock(output_mutex_);
    output_ = std::move(callback);
  }

  std::vector<std::string> SteamControllerDevice::describe_new_forward(const std::vector<std::uint8_t> &report) {
    // Logged once per distinct value, so a log shows what Steam changed on
    // the physical controller without repeating itself.
    std::vector<std::string> notes;
    auto note = [&](std::uint32_t key, std::string text) {
      if (forwarded_logged_.size() < 256 && forwarded_logged_.insert(key).second) {
        notes.push_back(std::move(text));
      }
    };
    const std::uint8_t command = report[1];
    if (command == kCmdSetSettingsValues && report.size() > 2) {
      const std::size_t end = std::min<std::size_t>(report.size(), 3u + report[2]);
      for (std::size_t i = 3; i + 3 <= end; i += 3) {
        const std::uint8_t setting = report[i];
        const std::uint16_t value = static_cast<std::uint16_t>(report[i + 1] | (report[i + 2] << 8));
        note((std::uint32_t {command} << 24) | (std::uint32_t {setting} << 16) | value,
             "Steam set setting " + std::to_string(setting) + " = " + std::to_string(value) + " (passed to the controller)");
      }
    } else {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "0x%02x", command);
      note(std::uint32_t {command} << 24, std::string("Steam sent command ") + hex + " (passed to the controller)");
    }
    return notes;
  }

  void SteamControllerDevice::set_blocked_settings(std::set<std::uint8_t> settings) {
    std::lock_guard lock(mutex_);
    blocked_settings_ = std::move(settings);
  }

  void SteamControllerDevice::emit_output(link::OutputKind kind, const std::vector<std::uint8_t> &report) {
    OutputCallback callback;
    {
      std::lock_guard lock(output_mutex_);
      callback = output_;
    }
    {
      std::lock_guard lock(mutex_);
      ++stats_.outputs_forwarded;
    }
    if (callback) {
      callback(kind, report);
    }
  }

  bool SteamControllerDevice::submit_report(const std::uint8_t *report, std::size_t length) {
    if (report == nullptr || length == 0) {
      return false;
    }

    {
      std::lock_guard lock(mutex_);
      ++stats_.reports_received;
      if (report[0] == kReportBattery) {
        if (length < kBatteryReportSize) {
          ++stats_.reports_dropped;
          return false;
        }
        queue_locked(std::vector<std::uint8_t>(report, report + kBatteryReportSize));
      } else {
        StateReport converted {};
        if (!converter_.to_wired(report, length, converted)) {
          ++stats_.reports_dropped;
          return false;
        }
        last_state_ = converted;
        have_state_ = true;
        queue_locked(std::vector<std::uint8_t>(converted.begin(), converted.end()));
      }
    }
    notify_in_ready();
    return true;
  }

  void SteamControllerDevice::release_all() {
    {
      std::lock_guard lock(mutex_);
      // Nothing pressed, sticks centred; keep the IMU clock moving forward.
      TritonStateUsb idle {};
      if (have_state_) {
        TritonStateUsb previous {};
        std::memcpy(&previous, last_state_.data() + 1, sizeof(previous));
        idle.controls.seq = static_cast<std::uint8_t>(previous.controls.seq + 1);
        idle.imu.timestamp_us = previous.imu.timestamp_us + kStateReportIntervalUs;
      }
      idle.imu.quat[0] = kIdentityQuatW;
      StateReport neutral {};
      neutral[0] = kReportState;
      std::memcpy(neutral.data() + 1, &idle, sizeof(idle));
      last_state_ = neutral;
      have_state_ = true;
      queue_locked(std::vector<std::uint8_t>(neutral.begin(), neutral.end()));
    }
    notify_in_ready();
  }

  void SteamControllerDevice::queue_locked(std::vector<std::uint8_t> report) {
    if (queue_.size() >= kMaxQueuedReports) {
      // Every state report carries full state, so the oldest one is expendable.
      queue_.pop_front();
      ++stats_.reports_dropped;
    }
    queue_.push_back(std::move(report));
  }

  SteamControllerDevice::Stats SteamControllerDevice::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
  }

  std::vector<std::uint8_t> SteamControllerDevice::device_descriptor() const {
    return {std::begin(triton_usb::kDeviceDescriptor), std::end(triton_usb::kDeviceDescriptor)};
  }

  std::vector<std::uint8_t> SteamControllerDevice::configuration_descriptor() const {
    return {std::begin(triton_usb::kConfigurationDescriptor), std::end(triton_usb::kConfigurationDescriptor)};
  }

  usbip::ControlResult SteamControllerDevice::control(const usbip::SetupPacket &setup, const std::vector<std::uint8_t> &out_data) {
    switch (setup.request_type & kTypeMask) {
      case kTypeStandard:
        return standard_request(setup);
      case kTypeClass:
        if ((setup.request_type & kRecipientMask) == kRecipientInterface && setup.index == 0) {
          return class_request(setup, out_data);
        }
        break;
      default:
        break;
    }
    return usbip::ControlResult::stalled();
  }

  usbip::ControlResult SteamControllerDevice::standard_request(const usbip::SetupPacket &setup) {
    const bool to_host = (setup.request_type & kDeviceToHost) != 0;
    const auto type = static_cast<std::uint8_t>(setup.value >> 8);
    const auto index = static_cast<std::uint8_t>(setup.value & 0xFF);

    switch (setup.request) {
      case kGetDescriptor:
        if (!to_host) {
          break;
        }
        switch (type) {
          case kDescDevice:
            return usbip::ControlResult::ok(device_descriptor());
          case kDescConfiguration:
            return usbip::ControlResult::ok(configuration_descriptor());
          case kDescString:
            if (index == 0) {
              return usbip::ControlResult::ok({4, kDescString, 0x09, 0x04});  // en-US
            }
            if (index == 1) {
              return usbip::ControlResult::ok(string_descriptor(triton_usb::kManufacturer));
            }
            if (index == 2) {
              return usbip::ControlResult::ok(string_descriptor(triton_usb::kProduct));
            }
            break;
          case kDescHid: {
            const auto *hid = triton_usb::kConfigurationDescriptor + triton_usb::kHidDescriptorOffset;
            return usbip::ControlResult::ok({hid, hid + triton_usb::kHidDescriptorLength});
          }
          case kDescHidReport:
            return usbip::ControlResult::ok({std::begin(triton_usb::kReportDescriptor), std::end(triton_usb::kReportDescriptor)});
          default:
            break;  // device qualifier, BOS...: a full-speed USB 2.0 device stalls these
        }
        break;
      case kGetStatus:
        return usbip::ControlResult::ok({0, 0});
      case kGetConfiguration:
        return usbip::ControlResult::ok({1});
      case kGetInterface:
        return usbip::ControlResult::ok({0});
      case kSetConfiguration:
      case kSetInterface:
      case kClearFeature:
      case kSetFeature:
        return usbip::ControlResult::ok();
      default:
        break;
    }
    return usbip::ControlResult::stalled();
  }

  usbip::ControlResult SteamControllerDevice::class_request(const usbip::SetupPacket &setup, const std::vector<std::uint8_t> &out_data) {
    const auto report_type = static_cast<std::uint8_t>(setup.value >> 8);
    const auto report_id = static_cast<std::uint8_t>(setup.value & 0xFF);

    switch (setup.request) {
      case kHidSetIdle:
      case kHidSetProtocol:
        return usbip::ControlResult::ok();
      case kHidGetIdle:
        return usbip::ControlResult::ok({0});
      case kHidGetProtocol:
        return usbip::ControlResult::ok({1});

      case kHidGetReport:
        if (report_type == kReportTypeFeature) {
          std::lock_guard lock(mutex_);
          const auto reply = responder_.on_get_feature(report_id);
          return usbip::ControlResult::ok({reply.begin(), reply.end()});
        }
        if (report_type == kReportTypeInput && report_id == kReportState) {
          std::lock_guard lock(mutex_);
          StateReport current = last_state_;
          current[0] = kReportState;
          return usbip::ControlResult::ok({current.begin(), current.end()});
        }
        break;

      case kHidSetReport: {
        const auto report = with_report_id(report_id, out_data);
        if (report_type == kReportTypeFeature) {
          FeatureDisposition disposition;
          bool first_time = false;
          std::vector<std::string> forwarded_notes;
          std::vector<std::uint8_t> forward;
          {
            std::lock_guard lock(mutex_);
            disposition = responder_.on_set_feature(report.data(), report.size());
            if (disposition == FeatureDisposition::kForward) {
              forward = without_settings(report, blocked_settings_);
              if (forward.size() != report.size() && report.size() > 2 && report[1] == kCmdSetSettingsValues) {
                for (std::size_t i = 3; i + 3 <= report.size() && i < 3u + report[2]; i += 3) {
                  if (blocked_settings_.count(report[i]) != 0 && forwarded_logged_.insert(0xFF000000u | report[i]).second) {
                    forwarded_notes.push_back("kept setting " + std::to_string(report[i]) + " from the physical controller (safety filter)");
                  }
                }
              }
              if (forward.size() > 1) {
                auto notes = describe_new_forward(forward);
                forwarded_notes.insert(forwarded_notes.end(), notes.begin(), notes.end());
              }
            }
            if (disposition == FeatureDisposition::kBlocked) {
              ++stats_.features_blocked;
              if (report.size() > 1 && !blocked_logged_[report[1]]) {
                blocked_logged_[report[1]] = true;
                first_time = true;
              }
            }
          }
          for (const auto &note : forwarded_notes) {
            log::info("steam controller: ", note);
          }
          if (disposition == FeatureDisposition::kForward) {
            if (report.size() > 1 && report[1] == kCmdTurnOffController) {
              log::info("steam controller: Steam turned the controller off");
            }
            if (!forward.empty()) {
              emit_output(link::OutputKind::kSetFeature, forward);
            }
          } else if (first_time) {
            // Expected: Steam sends commands the safety filter answers
            // locally instead of passing them to the physical controller.
            log::info("steam controller: kept command 0x", std::hex, int(report[1]), " from the physical controller (safety filter; this is normal)");
          }
          return usbip::ControlResult::ok();
        }
        if (report_type == kReportTypeOutput && is_output_report(report_id)) {
          emit_output(link::OutputKind::kOutputReport, report);
          return usbip::ControlResult::ok();
        }
        break;
      }
      default:
        break;
    }
    return usbip::ControlResult::stalled();
  }

  void SteamControllerDevice::interrupt_out(std::uint8_t endpoint, const std::vector<std::uint8_t> &data) {
    if (endpoint != kInterruptOutEndpoint || data.empty() || !is_output_report(data[0])) {
      return;
    }
    emit_output(link::OutputKind::kOutputReport, data);
  }

  bool SteamControllerDevice::pop_interrupt_in(std::uint8_t endpoint, std::vector<std::uint8_t> &report) {
    if (endpoint != kInterruptInEndpoint) {
      return false;
    }
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    report = std::move(queue_.front());
    queue_.pop_front();
    ++stats_.reports_delivered;
    return true;
  }

}  // namespace inputline
