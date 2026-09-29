# Research notes

Background that shaped the design, collected in September 2026. It describes the state of other projects at that time; check upstream for anything newer.

## Landscape (September 2026)

### The controller

| | |
|---|---|
| Released | 4 May 2026 (Valve's codename "Triton"; later hardware revision "Ibex") |
| Inputs | 2 TMR sticks with capacitive touch, 2 pressure-sensitive trackpads, 6-axis IMU, 4 back buttons, grip-sense capacitive sensors, QAM button |
| Links | 2.4 GHz Puck (`28DE:1304`), Bluetooth LE (`28DE:1303`), USB-C (`28DE:1302`) |
| USB protocol | **One** HID interface: state report `0x42` (54 B, ~250 Hz), battery `0x43`, haptic output reports `0x80`–`0x89`, command channel on feature reports 1 and 2 |
| BLE protocol | Valve GATT service `100F6C32-…`. State arrives as notifications of report `0x45` (older firmware) or `0x47` (newer, adds a trackpad timestamp). Each output report has its own characteristic. |

### Related software

| Project | Status | What it means for us |
|---|---|---|
| **SDL** (Valve-authored drivers) | `SDL_hidapi_steam_triton.c` is complete. The iOS/tvOS CoreBluetooth backend (`src/hidapi/ios/hid.m`) handles `0x1303` with both `0x45` and `0x47`. It's on `main` and the `release-3.4.x` branch, **not** in the 3.4.8 tag. | The client-side Bluetooth driver already exists, and it's the same code path Steam Link uses. |
| **moonlight-ios** | GameController-only. It bundles SDL **2.28.5** for audio only, which has no Triton support. Last commit Oct 2025. | It can't read the controller, so a separate client is needed. |
| **moonlight-qt** | Already reports `LI_CTYPE_STEAM` and dual touchpads via SDL. | Only useful for desktop and handheld clients. It confirms the upstream direction. |
| **moonlight-common-c** | Added `LI_CTYPE_STEAM` (Aug 2026), plus `LI_CCAP_DUAL_TOUCHPAD` and `LiSendControllerTouchEvent2` (May 2026). | The "semantic" path is partly standardised. Raw passthrough isn't. |
| **Sunshine** | Draft PR **#5773** adds a `steam2026` virtual gamepad through **libvirtualhid** (its PR #151). It rebuilds report `0x42` from Moonlight's normalised state. Haptic output is currently dropped. | Validates the idea. But the libvirtualhid Windows driver **requires a paid license**. |
| **Vibepollo** | Apollo fork. The public `master` still only has ViGEmBus (Xbox 360 / DS4). Release builds since the 1.19.0 betas (late August 2026) also ship Nonary's own VHF driver, **libvirtualgamepad** (see next row). Its moonlight-common-c is pinned at `2600bea`, so it has dual-touchpad support but no `LI_CTYPE_STEAM`. | Needs a Steam Controller-capable backend. Neither ViGEmBus nor libvirtualgamepad is one today. |
| **libvirtualgamepad** (Nonary; used by Vibeshine and Vibepollo) | User-mode VHF (UMDF2) driver. Profiles: Xbox Series, DS4, DualSense, Switch Pro. Takes **normalised** state only (`ioctl_submit_input_state`); raw feedback payloads are capped at 12 bytes. The project only accepts an identity like a VID/PID once the full emulation behind it has been tested. | The natural long-term home for a `steam_controller_2026` profile, since Vibepollo already installs that driver. It would need a raw input-report path and bigger feedback payloads, and it's unproven that Steam claims a HID-only Triton. |
| **HIDMaestro** (MIT, free) | `steam-controller-2` persona: `28DE:1302`, verbatim 372-byte HID descriptor, and the exact feature answers Steam validates before claiming a device. Uses an in-process USB/IP server over Microsoft-signed **usbip-win2**. Haptic writes arrive as output events. | **The best reference for the host.** Usable directly as a sidecar, or ported to C++. |
| VIIPER (GPL-3 core, MIT clients, Go) | USB/IP-based like HIDMaestro's composite personas, on the same Microsoft-signed usbip-win2. Devices (checked in source, Sep 2026): Xbox 360, DS4, DualSense/Edge, Switch 2 Pro, keyboard, mouse. **No Steam Controller.** Has a C++ header-only client and an embeddable `libVIIPER`. | A valid ViGEmBus replacement for *standard* pads. For this project it's only an alternative way to host the USB/IP persona: it would need a Triton device written in Go (~500 lines, using HIDMaestro's descriptors). |
| WinUHid (MIT, by Moonlight's author) | Clean C API over Microsoft's VHF. No signed release yet, and it's HID-only (no USB identity). | Possible later. It's unproven that Steam claims a HID-only Triton. |
| OpenPuck / ReversePuck | nRF52840 firmware that emulates the controller over USB; Steam accepts it. | A possible hardware fallback: a dongle that plays the controller over USB. |
| Apollo #1490, moonlight-qt #1881 | Users asking for exactly this. Apollo closed it as "not planned". | Nobody else offered the iOS side. |

---

## Semantic mapping vs raw passthrough

There are two ways to carry the controller across the network.

- **Semantic:** translate it into Moonlight's normal gamepad messages (buttons, sticks, touch events, motion events). This is what Sunshine's draft PR does.
- **Raw:** forward the controller's own HID reports byte for byte. This is what Steam Link does.

| | Semantic (Moonlight messages) | **Raw HID passthrough** |
|---|---|---|
| Gyro | Separate accel/gyro packets with no timestamps. moonlight-common-c keeps only the **latest** sample, and Vibepollo asks for **100 Hz**. | Native ~250 Hz frames carrying the controller's own IMU clock. Steam Input integrates them exactly as it would locally. |
| Triggers | 8-bit | 15-bit, plus the digital click bit |
| Trackpads | Floats per touch event, sent separately from buttons | Same frame as buttons and IMU, with pressure. Steam's trackpad smoothing gets the timing it expects. |
| Capacitive / grip sense | Needs new, non-standard button bits (Sunshine #5773 draft) | Free |
| Trackpad haptics | Needs a new message per haptic type. Nobody has built this yet. | Forward output reports `0x80`–`0x89` verbatim |
| Future firmware changes | Protocol changes on both ends | Only the host-side converter changes |
| Works with stock hosts and apps | ✅ | ✅ with Couchlink's side channel: the streaming host and app stay unmodified; `couchlink-host` runs next to them |

**Decision: raw passthrough over a separate link.** A small app on the device reads the controller and forwards its raw reports to `couchlink-host`, next to whichever streaming app is in front. Nothing in the streaming app or host changes.

---

## Sources

- SDL: [`SDL_hidapi_steam_triton.c`](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam_triton.c), [`hidapi/ios/hid.m`](https://github.com/libsdl-org/SDL/blob/main/src/hidapi/ios/hid.m), [`controller_structs.h`](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_structs.h), [SDL #15471 Steam Controller (2026) support](https://github.com/libsdl-org/SDL/issues/15471), [SDL #15528 touchpads / capsense](https://github.com/libsdl-org/SDL/pull/15528)
- Moonlight: [moonlight-common-c `Limelight.h`](https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/Limelight.h) (`LI_CTYPE_STEAM`, PR #148), [moonlight-qt `gamepad.cpp`](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/input/gamepad.cpp), [moonlight-ios `ControllerSupport.m`](https://github.com/moonlight-stream/moonlight-ios/blob/master/Limelight/Input/ControllerSupport.m), [moonlight-qt #1881](https://github.com/moonlight-stream/moonlight-qt/issues/1881), [moonlight-qt #393](https://github.com/moonlight-stream/moonlight-qt/issues/393)
- Hosts: [Vibepollo](https://github.com/Nonary/Vibepollo), [Vibepollo #462 (VHF gamepad driver)](https://github.com/Nonary/Vibepollo/issues/462), [Vibeshine](https://github.com/Nonary/vibeshine), [libvirtualgamepad](https://github.com/Nonary/libvirtualgamepad), [Apollo #1490](https://github.com/ClassicOldSong/Apollo/issues/1490), [Sunshine PR #5773](https://github.com/LizardByte/Sunshine/pull/5773), [Sunshine PR #4948 (WinUHid)](https://github.com/LizardByte/Sunshine/pull/4948), [libvirtualhid](https://github.com/LizardByte/libvirtualhid) and its PR #151
- Virtual devices: [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) (`profiles/valve/steam-controller-2.json`), [WinUHid](https://github.com/cgutman/WinUHid), [VIIPER](https://github.com/Alia5/VIIPER), [usbip-win2](https://github.com/vadimgrn/usbip-win2)
- Protocol research: [OpenPuck](https://github.com/safijari/openpuck), [CouchTurtle/sc2-research](https://github.com/CouchTurtle/sc2-research), [SteamDatabase Protobufs (`steammessages_remoteplay.proto`)](https://github.com/SteamDatabase/Protobufs)
- Hardware: [PC Gamer review](https://www.pcgamer.com/hardware/game-pads/steam-controller-2026-review/), [SteamHardware.io specs](https://steamhardware.io/steam-controller/specs/)
