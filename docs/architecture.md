# Architecture

## The problem

The 2026 Steam Controller only reaches its full potential through **Steam Input**, which runs inside the Steam client on the PC.

- **Steam Link** gets there by reading the controller's raw HID reports over Bluetooth and tunnelling them to the PC (`k_EStreamControlRemoteHID` in Valve's Remote Play protocol).
- **Other streaming apps**, such as Moonlight, only know Apple `GCController` gamepads on iOS and tvOS, so they cannot read the controller.
- **Sunshine-based hosts** can only create standard virtual pads (Xbox, DualShock, DualSense, Switch Pro).

## The approach

Do what Steam Link does, next to any streaming app:

1. **Read** the controller on the iPad or iPhone, in the Couchlink app, with Valve's own Bluetooth protocol (the one SDL implements). Couchlink keeps running in the background while the streaming app is in front.
2. **Forward** its raw reports, unchanged, to the PC over a small authenticated UDP link.
3. **Present** a virtual *wired* Steam Controller (`28DE:1302`) to Steam, with the exact USB identity and handshake of the real device, so Steam claims it as genuine hardware.
4. **Return** what Steam sends to the controller (haptics, settings) to the device, which writes it to the real controller.

```
 Steam Controller                iPad / iPhone                               Gaming PC
 ┌──────────────┐  BLE GATT   ┌─────────────────────────┐   UDP 48150   ┌────────────────────────────────────────┐
 │ state 0x45/47├────────────►│ CLKTritonBLE            │               │ couchlink-host                         │
 │              │             │   └► Couchlink link     │ Input ───────►│  LinkServer ─► SteamControllerDevice   │
 │              │◄────────────┤      (ClientSession)     │◄─── HidOutput│   (auth, sessions)   ├ report 0x45/47→0x42│
 │ haptics 0x8x │  write      │                         │               │                      ├ FeatureResponder  │
 └──────────────┘             └─────────────────────────┘               │                      └ haptics out       │
                                                                        │  usbip::Server (127.0.0.1:3240)        │
                                                                        │     ▲ OS USB/IP client attaches        │
                                                                        │     │ (usbip-win2 / vhci-hcd)          │
                                                                        │  Steam client ─► Steam Input ─► game   │
                                                                        └────────────────────────────────────────┘
```

## Why raw reports instead of the streaming app's gamepad messages

Streaming protocols such as Moonlight's can carry buttons, sticks, touch and motion. For this controller they lose what makes it special:

| | Streaming gamepad messages | Raw reports (this project) |
|---|---|---|
| Gyro | Separate accel/gyro packets, no sample timestamps. Clients typically keep only the latest sample; hosts typically ask for 100 Hz. | Every frame carries the controller's own IMU clock, so Steam's gyro integration sees real sample spacing. |
| Triggers | 8 bit | 15 bit, plus the click bit |
| Trackpads | Separate touch events | In the same frame as buttons and IMU, with pressure |
| Capacitive sticks and grips | Needs non-standard extension bits | Included |
| Trackpad haptics | Not in the protocol | Output reports forwarded verbatim |

## Why a side channel instead of changing the streaming host

The link runs beside the stream, not inside it. So neither the streaming app nor the streaming host (Vibepollo, Apollo, Sunshine...) needs a fork or a rebuild, and both keep their normal updates.

The cost is one small extra program on the PC, one app on the device, and a one-time pairing.

## Why USB/IP

Steam identifies a Steam Controller by its full USB identity and a feature-report handshake, not just its VID/PID. Creating such a device on Windows needs a driver. Options considered:

| Option | Verdict |
|---|---|
| **USB/IP via usbip-win2** | ✅ Chosen. The driver is Microsoft-signed, so no test mode or certificates. Our code stays in user space and presents a complete USB device, exactly as HIDMaestro's `steam-controller-2` persona does. |
| Custom KMDF/VHF driver | Needs an EV certificate and WHQL signing, or test-signing mode (which anti-cheat dislikes). |
| Virtual gamepad drivers used by streaming hosts | Built for standard gamepads from normalised state, not a raw Steam Controller with its full USB identity. |

The same USB/IP server also works on Linux with the in-kernel `vhci-hcd`. That's how CI tests the whole path end to end.

## Report handling

- **Input.** The client forwards `0x45` or `0x47` Bluetooth frames. `StateReportConverter` repacks them into the wired `0x42` layout Steam reads from `28DE:1302`:
  - It drops the `0x47` trackpad timestamp.
  - It unwraps the 16-bit, 32 µs IMU clock into a continuous microsecond counter.
  - It writes an identity quaternion, since Bluetooth frames carry none.
- **Identity.** `FeatureResponder` answers `GET_ATTRIBUTES_VALUES`, `GET_STRING_ATTRIBUTE`, wireless state and the settings read-back locally, with values read from real hardware. Waiting for a network round trip would stall Steam's claim.
- **Safety.** Commands are classified as *answer locally*, *forward* (settings, haptic pulses, IMU reset, turning the controller off, and gyro, stick, trigger and trackpad calibration, which run on the controller itself) or *blocked*. Blocked covers firmware and audio updates (the bootloader can't run over the link), factory reset (it wipes the Bluetooth pairing mid-session), serial/pairing/radio writes, turning keyboard/mouse emulation back on (double input on the device), and anything unknown. Blocked commands never reach the physical controller; Steam gets a reply from the virtual controller.
- **Output.** Haptic output reports (`0x80`–`0x89`), whether written to the interrupt OUT endpoint or via `SET_REPORT`, go back to the client verbatim.

## Threads

**couchlink-host**
- One UDP thread runs the link server.
- The USB/IP server has an accept thread plus one thread per attached device.
- Input frames are queued per device (bounded; the oldest frame is dropped, since each carries full state). They complete pending interrupt-IN URBs as they arrive.

**Couchlink app**
- CoreBluetooth runs on a dedicated high-priority serial queue, with state restoration so iOS can relaunch the app for a controller in the background.
- The link runs on its own serial queue. Each input datagram carries the report and the one before it, with a per-controller sequence number, so the PC recovers a single lost datagram and drops anything older than what it already has.
- The screen reads a status snapshot on the main thread and never waits on the link queue.

## Latency budget

| Hop | Typical |
|---|---|
| Controller → iPad / iPhone (BLE) | 15 ms connection interval on iOS. The same hop Steam Link pays. |
| Wi-Fi / Ethernet to the PC | ~1–3 ms on a LAN |
| couchlink-host → virtual device → Steam | < 1 ms (loopback USB/IP) |

Input latency ends up close to Steam Link's. Video and audio use the streaming app's own pipeline.
