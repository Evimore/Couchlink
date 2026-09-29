/**
 * @file feature_responder.h
 * @brief Host-side handling of the controller's feature-report command channel.
 *
 * Before Steam claims a 28DE:1302 device it interrogates it over feature
 * report 1: SET_FEATURE carries a command, the following GET_FEATURE reads the
 * answer. Waiting a network round trip for each answer would stall the claim,
 * so the host answers identity queries locally. Commands that change live
 * controller behaviour (settings, haptics) are also forwarded to the real
 * controller. Anything that writes flash, re-pairs radios or updates firmware
 * is never forwarded.
 *
 * Default identity values come from HIDMaestro's steam-controller-2 profile
 * (MIT, https://github.com/hifihedgehog/HIDMaestro), which cross-checked them
 * against two independent reads of real hardware.
 */
#pragma once

#include "triton.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace couchlink {

  /** Command IDs carried in byte 1 of feature report 1 (SDL controller_constants.h). */
  enum FeatureCommand : std::uint8_t {
    kCmdClearDigitalMappings = 0x81,
    kCmdGetDigitalMappings = 0x82,
    kCmdGetAttributesValues = 0x83,
    kCmdGetAttributeLabel = 0x84,
    kCmdFactoryReset = 0x86,
    kCmdSetSettingsValues = 0x87,
    kCmdClearSettingsValues = 0x88,
    kCmdGetSettingsValues = 0x89,
    kCmdGetSettingLabel = 0x8A,
    kCmdGetSettingsMaxs = 0x8B,
    kCmdGetSettingsDefaults = 0x8C,
    kCmdLoadDefaultSettings = 0x8E,
    kCmdTriggerHapticPulse = 0x8F,
    kCmdTurnOffController = 0x9F,
    kCmdGetDeviceInfo = 0xA1,
    kCmdGetStringAttribute = 0xAE,
    kCmdDongleGetWirelessState = 0xB4,
    kCmdCalibrateGyro = 0xB5,
    kCmdGetChipId = 0xBA,
    kCmdDongleGetConnectedSlots = 0xC4,
    kCmdResetImu = 0xCE,
  };

  /** What the host should do with a SET_FEATURE it received from Steam. */
  enum class FeatureDisposition {
    kAnswerLocally,  ///< Pure query: reply from the local model only.
    kForward,  ///< Changes live behaviour: reply locally and also send to the controller.
    kBlocked,  ///< Unsafe over a virtual link: reply locally, never forward.
  };

  /** Identity the virtual controller reports to Steam. */
  struct ControllerIdentity {
    /** Complete 64-byte reply to GET_ATTRIBUTES_VALUES, report ID first. */
    std::array<std::uint8_t, kFeatureReportSize> attributes_reply;
    std::string board_serial;  ///< GET_STRING_ATTRIBUTE index 0
    std::string unit_serial;  ///< GET_STRING_ATTRIBUTE index 1
    std::string valve_constant;  ///< GET_STRING_ATTRIBUTE index 3, identical on every unit
  };

  /**
   * @brief Identity grounded on real 28DE:1302 hardware reads.
   * @param instance Distinguishes serials when several controllers are attached (0-9).
   */
  ControllerIdentity default_identity(std::uint8_t instance = 0);

  /**
   * SET_SETTINGS_VALUES setting for the wireless protocol version. It is radio
   * configuration, meaningless for a controller reached over Bluetooth, so
   * it is not passed on by default.
   */
  constexpr std::uint8_t kSettingWirelessPacketVersion = 49;

  /**
   * @brief Drop some settings from a SET_SETTINGS_VALUES (0x87) report before it
   *        reaches the physical controller.
   * @return The report to forward (unchanged if it is another command), or an
   *         empty vector if no setting is left.
   */
  std::vector<std::uint8_t> without_settings(const std::vector<std::uint8_t> &report, const std::set<std::uint8_t> &blocked);

  /** GET_ATTRIBUTES_VALUES record tags. */
  constexpr std::uint8_t kAttribFirmwareBuild = 0x04;
  constexpr std::uint8_t kAttribBootloaderBuild = 0x0A;

  /**
   * @brief Report the physical controller's firmware and bootloader builds.
   *
   * @p reply is the controller's own GET_ATTRIBUTES_VALUES reply (report ID
   * first), read by the client over Bluetooth. Only the build records are
   * copied, so Steam compares its firmware against the real one instead of
   * the built-in identity's (and stops offering an update it cannot apply).
   * @return true if anything was copied.
   */
  bool adopt_firmware_attributes(ControllerIdentity &identity, const std::array<std::uint8_t, kFeatureReportSize> &reply);

  class FeatureResponder {
  public:
    explicit FeatureResponder(ControllerIdentity identity = default_identity());

    /**
     * @brief Record a SET_FEATURE from Steam and classify it.
     * @param data Report bytes, report ID first.
     */
    FeatureDisposition on_set_feature(const std::uint8_t *data, std::size_t length);

    /**
     * @brief Produce the answer to a GET_FEATURE from Steam.
     * @return The 64-byte reply, report ID first.
     */
    std::array<std::uint8_t, kFeatureReportSize> on_get_feature(std::uint8_t report_id) const;

    /** Classify a command without recording it. */
    static FeatureDisposition classify(std::uint8_t command);

  private:
    ControllerIdentity identity_;
    std::array<std::vector<std::uint8_t>, 2> last_request_;  // per feature report 1 and 2
  };

}  // namespace couchlink
