/**
 * @file triton.h
 * @brief Wire layouts of the 2026 Steam Controller ("Triton") HID reports.
 *
 * Every layout here mirrors SDL's Valve-authored definitions in
 * src/joystick/hidapi/steam/controller_structs.h and
 * src/joystick/hidapi/SDL_hidapi_steam_triton.c (SDL main, September 2026).
 * All multi-byte fields are little-endian on the wire.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace inputline {

  constexpr std::uint16_t kValveVendorId = 0x28DE;
  constexpr std::uint16_t kTritonUsbProductId = 0x1302;  ///< Wired controller.
  constexpr std::uint16_t kTritonBleProductId = 0x1303;  ///< Bluetooth LE controller.
  constexpr std::uint16_t kProteusDongleProductId = 0x1304;  ///< Steam Controller Puck.
  constexpr std::uint16_t kNereidDongleProductId = 0x1305;

  /** Input and feature report IDs used by the controller. */
  enum ReportId : std::uint8_t {
    kReportFeatureChannel1 = 0x01,  ///< 63-byte command channel (SET/GET_FEATURE).
    kReportFeatureChannel2 = 0x02,
    kReportLizardMouse = 0x40,
    kReportLizardKeyboard = 0x41,
    kReportState = 0x42,  ///< Wired state report, what Steam reads from 28DE:1302.
    kReportBattery = 0x43,
    kReportStateBle = 0x45,  ///< Bluetooth state report (IMU without quaternion).
    kReportWirelessStatusX = 0x46,
    kReportStateTimestamped = 0x47,  ///< Newer Bluetooth state report with a trackpad timestamp.
    kReportWirelessStatus = 0x79,
  };

  /** Output reports Steam writes for rumble and trackpad haptics (0x80-0x89). */
  enum OutputReportId : std::uint8_t {
    kOutputHapticRumble = 0x80,
    kOutputHapticPulse = 0x81,
    kOutputHapticCommand = 0x82,
    kOutputHapticLfoTone = 0x83,
    kOutputHapticLogSweep = 0x84,
    kOutputHapticScript = 0x85,
    kOutputReportLast = 0x89,
  };

  /** Button bits of the 32-bit `buttons` field, as named by SDL. */
  enum Button : std::uint32_t {
    kButtonA = 0x00000001,
    kButtonB = 0x00000002,
    kButtonX = 0x00000004,
    kButtonY = 0x00000008,
    kButtonQam = 0x00000010,
    kButtonR3 = 0x00000020,
    kButtonView = 0x00000040,
    kButtonR4 = 0x00000080,
    kButtonR5 = 0x00000100,
    kButtonRB = 0x00000200,
    kButtonDpadDown = 0x00000400,
    kButtonDpadRight = 0x00000800,
    kButtonDpadLeft = 0x00001000,
    kButtonDpadUp = 0x00002000,
    kButtonMenu = 0x00004000,
    kButtonL3 = 0x00008000,
    kButtonSteam = 0x00010000,
    kButtonL4 = 0x00020000,
    kButtonL5 = 0x00040000,
    kButtonLB = 0x00080000,
    kRightStickTouch = 0x00100000,
    kRightPadTouch = 0x00200000,
    kRightPadClick = 0x00400000,
    kRightTriggerClick = 0x00800000,
    kLeftStickTouch = 0x01000000,
    kLeftPadTouch = 0x02000000,
    kLeftPadClick = 0x04000000,
    kLeftTriggerClick = 0x08000000,
    kRightGripTouch = 0x10000000,
    kLeftGripTouch = 0x20000000,
  };

#pragma pack(push, 1)

  /** Fields shared by every state report, up to the sticks. */
  struct TritonControls {
    std::uint8_t seq;
    std::uint32_t buttons;
    std::int16_t trigger_left;  ///< 0..32767
    std::int16_t trigger_right;  ///< 0..32767
    std::int16_t left_stick_x;
    std::int16_t left_stick_y;  ///< Up is positive.
    std::int16_t right_stick_x;
    std::int16_t right_stick_y;  ///< Up is positive.
  };

  struct TritonPads {
    std::int16_t left_x;
    std::int16_t left_y;
    std::uint16_t left_pressure;
    std::int16_t right_x;
    std::int16_t right_y;
    std::uint16_t right_pressure;
  };

  struct TritonImuFull {
    std::uint32_t timestamp_us;
    std::int16_t accel[3];
    std::int16_t gyro[3];
    std::int16_t quat[4];  ///< W, X, Y, Z
  };

  struct TritonImuNoQuat {
    std::uint32_t timestamp_us;
    std::int16_t accel[3];
    std::int16_t gyro[3];
  };

  struct TritonImuNoQuat16 {
    std::uint16_t timestamp_32us;  ///< Wrapping counter in 32 microsecond units.
    std::int16_t accel[3];
    std::int16_t gyro[3];
  };

  /** Payload of report 0x42 (TritonMTUFull_t). */
  struct TritonStateUsb {
    TritonControls controls;
    TritonPads pads;
    TritonImuFull imu;
  };

  /** Payload of report 0x45 (TritonMTUNoQuat_t). */
  struct TritonStateBle {
    TritonControls controls;
    TritonPads pads;
    TritonImuNoQuat imu;
  };

  /** Payload of report 0x47 (TritonMTUNoQuat32TS_t). */
  struct TritonStateTimestamped {
    TritonControls controls;
    std::uint16_t trackpad_timestamp;
    TritonPads pads;
    TritonImuNoQuat16 imu;
  };

#pragma pack(pop)

  static_assert(sizeof(TritonControls) == 17);
  static_assert(sizeof(TritonPads) == 12);
  static_assert(sizeof(TritonStateUsb) == 53);
  static_assert(sizeof(TritonStateBle) == 45);
  static_assert(sizeof(TritonStateTimestamped) == 45);

  /** Sizes including the leading report ID byte. */
  constexpr std::size_t kStateReportSize = 1 + sizeof(TritonStateUsb);  // 54
  constexpr std::size_t kBleStateReportSize = 1 + sizeof(TritonStateBle);  // 46
  constexpr std::size_t kFeatureReportSize = 64;
  constexpr std::size_t kMaxReportSize = 64;

  /** Nominal interval between state reports (SDL: "about 4 ms"). */
  constexpr std::uint32_t kStateReportIntervalUs = 4032;

}  // namespace inputline
