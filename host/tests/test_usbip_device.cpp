// Drives the USB/IP server and the virtual Steam Controller over real
// loopback TCP, playing the part usbip-win2 or vhci-hcd play in production.

#include "check.h"
#include "log.h"
#include "steam_controller_device.h"
#include "triton_descriptors.h"
#include "usbip_attach.h"
#include "usbip_test_client.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

using namespace inputline;

namespace {

  std::vector<std::uint8_t> ble_state_report(std::uint32_t buttons, std::uint32_t timestamp_us) {
    TritonStateBle ble {};
    ble.controls.buttons = buttons;
    ble.controls.left_stick_x = 1000;
    ble.imu.timestamp_us = timestamp_us;
    ble.imu.gyro[1] = 77;
    std::vector<std::uint8_t> report(kBleStateReportSize);
    report[0] = kReportStateBle;
    std::memcpy(report.data() + 1, &ble, sizeof(ble));
    return report;
  }

  struct Captured {
    std::mutex mutex;
    std::vector<std::pair<link::OutputKind, std::vector<std::uint8_t>>> outputs;

    std::size_t size() {
      std::lock_guard lock(mutex);
      return outputs.size();
    }
  };

  bool wait_for(const std::function<bool()> &condition) {
    for (int i = 0; i < 200; ++i) {
      if (condition()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  void test_descriptors_and_handshake() {
    usbip::Server server;
    CHECK(server.start("127.0.0.1", 0));
    auto device = std::make_shared<SteamControllerDevice>();
    Captured captured;
    device->set_output_callback([&](link::OutputKind kind, const std::vector<std::uint8_t> &report) {
      std::lock_guard lock(captured.mutex);
      captured.outputs.emplace_back(kind, report);
    });
    const auto busid = server.add_device(device);
    CHECK(busid == "1-1");

    test::UsbipTestClient client(server.port());
    const auto list = client.devlist();
    CHECK(list.size() == 1);
    CHECK(!list.empty() && list[0].busid == "1-1" && list[0].vendor == 0x28DE && list[0].product == 0x1302);
    CHECK(!list.empty() && list[0].num_interfaces == 1 && list[0].interface_class == 3);

    std::uint16_t vendor = 0, product = 0;
    CHECK(client.import(busid, &vendor, &product));
    CHECK(vendor == 0x28DE && product == 0x1302);
    CHECK(wait_for([&] {
      return server.is_attached(busid);
    }));
    CHECK(client.devlist().empty());  // attached devices are no longer exportable

    // Device descriptor, first 8 bytes then all of it, as Windows does.
    auto reply = client.control(0x80, 0x06, 0x0100, 0, 8);
    CHECK(reply && reply->status == 0 && reply->data.size() == 8 && reply->data[7] == 64);
    reply = client.control(0x80, 0x06, 0x0100, 0, 18);
    CHECK(reply && reply->data.size() == 18 &&
          std::memcmp(reply->data.data(), triton_usb::kDeviceDescriptor, 18) == 0);

    reply = client.control(0x80, 0x06, 0x0200, 0, 9);
    CHECK(reply && reply->data.size() == 9 && reply->data[2] == 41);
    reply = client.control(0x80, 0x06, 0x0200, 0, 255);
    CHECK(reply && reply->data.size() == 41);

    reply = client.control(0x80, 0x06, 0x0302, 0x0409, 255);
    CHECK(reply && reply->data.size() == 2 + 2 * 16 && reply->data[2] == 'S' && reply->data[4] == 't');
    reply = client.control(0x80, 0x06, 0x0600, 0, 10);  // device qualifier
    CHECK(reply && reply->status == usbip::proto::kStatusStall);

    reply = client.control(0x00, 0x09, 1, 0, 0);  // SET_CONFIGURATION
    CHECK(reply && reply->status == 0);
    reply = client.control(0x21, 0x0A, 0, 0, 0);  // SET_IDLE
    CHECK(reply && reply->status == 0);

    reply = client.control(0x81, 0x06, 0x2200, 0, 372);
    CHECK(reply && reply->data.size() == 372 &&
          std::memcmp(reply->data.data(), triton_usb::kReportDescriptor, 372) == 0);

    // Steam's identity query: SET_FEATURE(0x83) then GET_FEATURE.
    std::vector<std::uint8_t> request(64, 0);
    request[0] = 1;
    request[1] = kCmdGetAttributesValues;
    reply = client.control(0x21, 0x09, 0x0301, 0, 64, request);
    CHECK(reply && reply->status == 0 && reply->actual_length == 64);
    reply = client.control(0xA1, 0x01, 0x0301, 0, 64);
    CHECK(reply && reply->data.size() == 64 && reply->data[1] == 0x83 && reply->data[2] == 0x19 && reply->data[4] == 0x02 && reply->data[5] == 0x13);
    CHECK(captured.size() == 0);  // identity queries never leave the host

    // Settings are answered locally and forwarded to the real controller.
    request[1] = kCmdSetSettingsValues;
    request[2] = 3;
    request[3] = 9;  // lizard mode
    reply = client.control(0x21, 0x09, 0x0301, 0, 64, request);
    CHECK(reply && reply->status == 0);
    reply = client.control(0xA1, 0x01, 0x0301, 0, 64);
    CHECK(reply && reply->data[1] == kCmdSetSettingsValues && reply->data[3] == 9);
    CHECK(wait_for([&] {
      return captured.size() == 1;
    }));
    {
      std::lock_guard lock(captured.mutex);
      CHECK(!captured.outputs.empty() && captured.outputs[0].first == link::OutputKind::kSetFeature &&
            captured.outputs[0].second.size() == 64 && captured.outputs[0].second[1] == kCmdSetSettingsValues);
    }

    // Factory reset never reaches the real controller.
    request[1] = kCmdFactoryReset;
    reply = client.control(0x21, 0x09, 0x0301, 0, 64, request);
    CHECK(reply && reply->status == 0);
    CHECK(device->stats().features_blocked == 1);
    CHECK(captured.size() == 1);

    // Unknown class request stalls.
    reply = client.control(0xA1, 0x42, 0, 0, 8);
    CHECK(reply && reply->status == usbip::proto::kStatusStall);

    client.disconnect();
    CHECK(wait_for([&] {
      return !server.is_attached(busid);
    }));
    server.stop();
  }

  void test_interrupt_transfers() {
    usbip::Server server;
    CHECK(server.start("127.0.0.1", 0));
    auto device = std::make_shared<SteamControllerDevice>();
    Captured captured;
    device->set_output_callback([&](link::OutputKind kind, const std::vector<std::uint8_t> &report) {
      std::lock_guard lock(captured.mutex);
      captured.outputs.emplace_back(kind, report);
    });
    const auto busid = server.add_device(device);

    test::UsbipTestClient client(server.port());
    CHECK(client.import(busid));

    // URB first, report later: completed as soon as the report arrives.
    client.expect_in_data(true);
    const auto first = client.submit(usbip::proto::kDirIn, 1, 64);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto ble = ble_state_report(kButtonA, 1000);
    CHECK(device->submit_report(ble.data(), ble.size()));
    auto reply = client.read_reply();
    CHECK(reply && reply->seqnum == first && reply->status == 0 && reply->data.size() == kStateReportSize);
    CHECK(reply && reply->data[0] == kReportState);
    if (reply && reply->data.size() == kStateReportSize) {
      TritonStateUsb state {};
      std::memcpy(&state, reply->data.data() + 1, sizeof(state));
      CHECK(state.controls.buttons == kButtonA && state.controls.left_stick_x == 1000 && state.imu.gyro[1] == 77);
      CHECK(state.imu.timestamp_us == 1000 && state.imu.quat[0] == kIdentityQuatW);
    }

    // Reports first, URBs later: delivered in order.
    const auto second_report = ble_state_report(kButtonB, 5032);
    const auto third_report = ble_state_report(kButtonX, 9064);
    device->submit_report(second_report.data(), second_report.size());
    device->submit_report(third_report.data(), third_report.size());
    client.submit(usbip::proto::kDirIn, 1, 64);
    client.submit(usbip::proto::kDirIn, 1, 64);
    auto r2 = client.read_reply();
    auto r3 = client.read_reply();
    CHECK(r2 && r3 && r2->data.size() == kStateReportSize && r3->data.size() == kStateReportSize);
    if (r2 && r3 && r2->data.size() == kStateReportSize && r3->data.size() == kStateReportSize) {
      TritonStateUsb s2 {}, s3 {};
      std::memcpy(&s2, r2->data.data() + 1, sizeof(s2));
      std::memcpy(&s3, r3->data.data() + 1, sizeof(s3));
      CHECK(s2.controls.buttons == kButtonB && s3.controls.buttons == kButtonX);
    }

    // Battery reports pass through untouched.
    std::vector<std::uint8_t> battery(SteamControllerDevice::kBatteryReportSize, 0);
    battery[0] = kReportBattery;
    battery[2] = 87;
    CHECK(device->submit_report(battery.data(), battery.size()));
    client.submit(usbip::proto::kDirIn, 1, 64);
    reply = client.read_reply();
    CHECK(reply && reply->data.size() == SteamControllerDevice::kBatteryReportSize && reply->data[2] == 87);

    // Unusable reports are rejected.
    const std::uint8_t junk[] = {0x7B, 1, 2};
    CHECK(!device->submit_report(junk, sizeof(junk)));

    // Haptics written to the interrupt OUT endpoint reach the client.
    client.expect_in_data(false);
    const std::vector<std::uint8_t> pulse = {kOutputHapticPulse, 1, 0x10, 0x00, 0x10, 0x00, 3, 0};
    client.submit(usbip::proto::kDirOut, 1, static_cast<std::uint32_t>(pulse.size()), nullptr, pulse);
    reply = client.read_reply();
    CHECK(reply && reply->status == 0 && reply->actual_length == static_cast<std::int32_t>(pulse.size()));
    CHECK(wait_for([&] {
      return captured.size() == 1;
    }));
    {
      std::lock_guard lock(captured.mutex);
      CHECK(!captured.outputs.empty() && captured.outputs[0].first == link::OutputKind::kOutputReport && captured.outputs[0].second == pulse);
    }

    // Unlinking a pending URB.
    client.expect_in_data(true);
    const auto pending = client.submit(usbip::proto::kDirIn, 1, 64);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client.unlink(pending);
    std::uint32_t command = 0;
    reply = client.read_reply(&command);
    CHECK(reply && command == usbip::proto::kRetUnlink && reply->status == usbip::proto::kStatusUnlinked);

    // A second importer is refused while attached.
    test::UsbipTestClient intruder(server.port());
    CHECK(!intruder.import(busid));

    // Removing the device unplugs it.
    server.remove_device(busid);
    reply = client.read_reply();
    CHECK(!reply.has_value());
    CHECK(!server.is_attached(busid));

    // release_all queues a neutral report.
    auto fresh = std::make_shared<SteamControllerDevice>();
    const auto pressed = ble_state_report(kButtonY, 10);
    fresh->submit_report(pressed.data(), pressed.size());
    std::vector<std::uint8_t> drained;
    CHECK(fresh->pop_interrupt_in(1, drained));
    fresh->release_all();
    CHECK(fresh->pop_interrupt_in(1, drained));
    if (drained.size() == kStateReportSize) {
      TritonStateUsb idle {};
      std::memcpy(&idle, drained.data() + 1, sizeof(idle));
      CHECK(idle.controls.buttons == 0 && idle.imu.timestamp_us == 10 + kStateReportIntervalUs);
    }

    // Queue overflow drops the oldest reports.
    for (int i = 0; i < 20; ++i) {
      fresh->submit_report(pressed.data(), pressed.size());
    }
    CHECK(fresh->stats().reports_dropped >= 12);

    server.stop();
  }

  void test_server_restart_and_multiple_devices() {
    usbip::Server server;
    CHECK(server.start("127.0.0.1", 0));
    const auto a = server.add_device(std::make_shared<SteamControllerDevice>(default_identity(0)));
    const auto b = server.add_device(std::make_shared<SteamControllerDevice>(default_identity(1)));
    CHECK(a == "1-1" && b == "1-2");
    test::UsbipTestClient client_a(server.port());
    test::UsbipTestClient client_b(server.port());
    CHECK(client_a.import(a));
    CHECK(client_b.import(b));
    CHECK(wait_for([&] {
      return server.is_attached(a) && server.is_attached(b);
    }));
    server.stop();  // must unblock both connection threads
    CHECK(!client_a.read_reply().has_value());
    server.stop();  // idempotent
  }

  /** usbip's messages reach the log only if run_process captures them. */
  void test_run_process_captures_output() {
#ifdef _WIN32
    const std::vector<std::string> argv {"cmd.exe", "/c", "echo out& echo err 1>&2& exit 3"};
#else
    const std::vector<std::string> argv {"sh", "-c", "echo out; echo err >&2; exit 3"};
#endif
    std::string output;
    CHECK(run_process(argv, &output) == 3);
    CHECK(output.find("out") != std::string::npos);
    CHECK(output.find("err") != std::string::npos);
    CHECK(run_process(argv) == 3);  // without capture
    CHECK(run_process({"inputline-no-such-program"}, &output) != 0);
  }

}  // namespace

int main() {
  net::startup();
  log::set_level(log::Level::kWarning);
  test_descriptors_and_handshake();
  test_interrupt_transfers();
  test_server_restart_and_multiple_devices();
  test_run_process_captures_output();
  return test::report_and_exit_code();
}
