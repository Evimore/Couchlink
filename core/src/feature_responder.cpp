#include "inputline/feature_responder.h"

#include <algorithm>
#include <cstring>

namespace inputline {

  namespace {
    constexpr std::size_t kStringValueBytes = 19;  // after the index byte, 20 bytes total
    constexpr std::uint8_t kUnprovisionedIndex = 0xFF;
    constexpr std::uint8_t kWiredNoRadio = 0x01;

    std::array<std::uint8_t, kFeatureReportSize> blank_reply(std::uint8_t report_id, std::uint8_t command) {
      std::array<std::uint8_t, kFeatureReportSize> reply {};
      reply[0] = report_id;
      reply[1] = command;
      return reply;
    }
  }  // namespace

  ControllerIdentity default_identity(std::uint8_t instance) {
    ControllerIdentity identity {};

    // GET_ATTRIBUTES_VALUES reply as (tag, u32 LE) records:
    // 01 product 0x1302, 02 capabilities 0, 0A bootloader build 0x68D2F92E,
    // 04 firmware build 0x6A18D057 (2026-05-28), 09 board revision 0x48.
    // Steam validates these byte for byte.
    constexpr std::uint8_t attributes[] = {
      0x01, 0x83, 0x19,
      0x01, 0x02, 0x13, 0x00, 0x00,
      0x02, 0x00, 0x00, 0x00, 0x00,
      0x0A, 0x2E, 0xF9, 0xD2, 0x68,
      0x04, 0x57, 0xD0, 0x18, 0x6A,
      0x09, 0x48, 0x00, 0x00, 0x00,
    };
    std::copy(std::begin(attributes), std::end(attributes), identity.attributes_reply.begin());

    const char digit = static_cast<char>('0' + (instance % 10));
    identity.board_serial = std::string("MXA996020000") + digit;
    identity.unit_serial = std::string("FXA996020000") + digit;
    identity.valve_constant = "7054257d2da7";
    return identity;
  }

  std::vector<std::uint8_t> without_settings(const std::vector<std::uint8_t> &report, const std::set<std::uint8_t> &blocked) {
    constexpr std::size_t kFirst = 3;
    if (report.size() < kFirst || report[1] != kCmdSetSettingsValues || blocked.empty()) {
      return report;
    }
    const std::size_t end = std::min<std::size_t>(report.size(), kFirst + report[2]);
    std::vector<std::uint8_t> kept(report.begin(), report.begin() + kFirst);
    for (std::size_t i = kFirst; i + 3 <= end; i += 3) {
      if (blocked.count(report[i]) == 0) {
        kept.insert(kept.end(), report.begin() + static_cast<std::ptrdiff_t>(i), report.begin() + static_cast<std::ptrdiff_t>(i + 3));
      }
    }
    if (kept.size() == end) {
      return report;  // nothing blocked
    }
    if (kept.size() == kFirst) {
      return {};
    }
    kept[2] = static_cast<std::uint8_t>(kept.size() - kFirst);
    kept.resize(report.size(), 0);  // keep the report length (feature reports are padded to 64 bytes)
    return kept;
  }

  bool adopt_firmware_attributes(ControllerIdentity &identity, const std::array<std::uint8_t, kFeatureReportSize> &reply) {
    constexpr std::size_t kRecord = 5;  // tag, u32 LE
    constexpr std::size_t kFirst = 3;
    if (reply[1] != kCmdGetAttributesValues) {
      return false;
    }
    auto &own = identity.attributes_reply;
    const std::size_t end = kFirst + std::min<std::size_t>(reply[2], kFeatureReportSize - kFirst);
    const std::size_t own_end = kFirst + std::min<std::size_t>(own[2], kFeatureReportSize - kFirst);
    bool copied = false;
    for (std::size_t i = kFirst; i + kRecord <= end; i += kRecord) {
      const std::uint8_t tag = reply[i];
      const bool empty = reply[i + 1] == 0 && reply[i + 2] == 0 && reply[i + 3] == 0 && reply[i + 4] == 0;
      if ((tag != kAttribFirmwareBuild && tag != kAttribBootloaderBuild) || empty) {
        continue;
      }
      for (std::size_t j = kFirst; j + kRecord <= own_end; j += kRecord) {
        if (own[j] == tag) {
          std::copy(reply.begin() + static_cast<std::ptrdiff_t>(i + 1), reply.begin() + static_cast<std::ptrdiff_t>(i + kRecord),
                    own.begin() + static_cast<std::ptrdiff_t>(j + 1));
          copied = true;
        }
      }
    }
    return copied;
  }

  bool is_lasting_setting(const std::vector<std::uint8_t> &report) {
    if (report.size() < 2) {
      return false;
    }
    switch (report[1]) {
      case kCmdClearDigitalMappings:
      case kCmdSetSettingsValues:
      case kCmdClearSettingsValues:
      case kCmdLoadDefaultSettings:
        return true;
      default:
        return false;
    }
  }

  FeatureResponder::FeatureResponder(ControllerIdentity identity):
      identity_(std::move(identity)) {}

  FeatureDisposition FeatureResponder::classify(std::uint8_t command) {
    switch (command) {
      case kCmdGetDigitalMappings:
      case kCmdGetAttributesValues:
      case kCmdGetAttributeLabel:
      case kCmdGetSettingsValues:
      case kCmdGetSettingLabel:
      case kCmdGetSettingsMaxs:
      case kCmdGetSettingsDefaults:
      case kCmdGetDeviceInfo:
      case kCmdGetStringAttribute:
      case kCmdDongleGetWirelessState:
      case kCmdGetChipId:
      case kCmdDongleGetConnectedSlots:
        return FeatureDisposition::kAnswerLocally;

      case kCmdClearDigitalMappings:  // turns keyboard/mouse emulation off
      case kCmdSetSettingsValues:
      case kCmdClearSettingsValues:
      case kCmdLoadDefaultSettings:
      case kCmdTriggerHapticPulse:
      case kCmdTurnOffController:  // Steam button + Y, "Turn off controller"
      case kCmdResetImu:
      // Calibration runs on the controller itself; a poor result is fixed by
      // calibrating again. Readings Steam asks for afterwards come from the
      // local model, not the controller.
      case kCmdCalibrateGyro:
      case kCmdCalibrateTrackpads:
      case kCmdCalibrateJoystick:
      case kCmdCalibrateAnalogTriggers:
      case kCmdCalibrateAnalog:
        return FeatureDisposition::kForward;

      default:
        // Firmware and audio updates (the bootloader can't run over the
        // link), factory reset (wipes the Bluetooth pairing mid-session),
        // serial/pairing/radio writes (the link depends on them), turning
        // keyboard/mouse emulation back on (double input on the device),
        // and anything unknown.
        return FeatureDisposition::kBlocked;
    }
  }

  FeatureDisposition FeatureResponder::on_set_feature(const std::uint8_t *data, std::size_t length) {
    if (data == nullptr || length < 2 ||
        (data[0] != kReportFeatureChannel1 && data[0] != kReportFeatureChannel2)) {
      return FeatureDisposition::kBlocked;
    }

    auto &slot = last_request_[data[0] - 1];
    slot.assign(data, data + std::min(length, kFeatureReportSize));
    slot.resize(kFeatureReportSize, 0);
    return classify(data[1]);
  }

  std::array<std::uint8_t, kFeatureReportSize> FeatureResponder::on_get_feature(std::uint8_t report_id) const {
    if (report_id != kReportFeatureChannel1 && report_id != kReportFeatureChannel2) {
      return blank_reply(report_id, 0);
    }

    const auto &request = last_request_[report_id - 1];
    if (request.size() < 2) {
      return blank_reply(report_id, 0);
    }

    const std::uint8_t command = request[1];
    auto reply = blank_reply(report_id, command);

    switch (command) {
      case kCmdGetAttributesValues:
        reply = identity_.attributes_reply;
        reply[0] = report_id;
        break;

      case kCmdGetStringAttribute: {
        const std::uint8_t index = request.size() > 3 ? request[3] : 0;
        const std::string *value = nullptr;
        if (index == 0) {
          value = &identity_.board_serial;
        } else if (index == 1) {
          value = &identity_.unit_serial;
        } else if (index == 3) {
          value = &identity_.valve_constant;
        }
        reply[2] = 1 + kStringValueBytes;
        reply[3] = value ? index : kUnprovisionedIndex;
        if (value) {
          std::memcpy(&reply[4], value->data(), std::min(value->size(), kStringValueBytes));
        }
        break;
      }

      case kCmdDongleGetWirelessState:
        reply[2] = 1;
        reply[3] = kWiredNoRadio;
        break;

      case kCmdSetSettingsValues:
        // A real unit reads back the settings block the host just wrote, and
        // Steam stalls the claim if this read does not complete.
        std::copy(request.begin(), request.end(), reply.begin());
        reply[0] = report_id;
        break;

      default:
        break;
    }
    return reply;
  }

}  // namespace inputline
