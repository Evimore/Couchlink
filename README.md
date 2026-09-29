<p align="center"><img src="docs/images/icon.png" width="128" height="128" alt="Couchlink icon"></p>

# Couchlink

**Your Steam Controller with full Steam Input on your gaming PC, while you stream to an iPad, iPhone or Apple TV, next to any streaming app.**

Steam Link supports the Steam Controller over Bluetooth because it forwards the controller's raw data to Steam on your PC. Other streaming apps, Moonlight included, can't. Couchlink adds that, without modifying them:

- **On the iPad / iPhone**, the Couchlink app reads the controller over Bluetooth and sends its raw reports to your PC. It keeps running in the background while you stream with Moonlight or any other app, unmodified.
- **On the PC**, `couchlink-host` plugs in a virtual *wired* Steam Controller that Steam recognises as the real thing.

You get everything Steam Input offers: both trackpads (with pressure and haptics), gyro, the four back buttons, capacitive stick and grip sensing, and your per-game configurations.

> **Status: early.** Works end to end with a real 2026 Steam Controller, an iPad and a Windows PC: Steam shows full Steam Input and every input works, with the same feel as Steam Link. The app isn't on the App Store yet; you install it yourself for now. See the [roadmap](docs/roadmap.md).

```
 iPad / iPhone                                        Gaming PC
┌──────────────────────────────┐            ┌──────────────────────────────────────┐
│ Steam Controller ──BLE──►    │  UDP link  │ couchlink-host                       │
│ Couchlink (in the background)│ ─────────► │   └► virtual USB Steam Controller    │
│                              │ ◄───────── │        (usbip-win2 / vhci-hcd)       │
│ (haptics played back)        │  haptics   │         └► Steam Input ─► your game  │
└──────────────────────────────┘            └──────────────────────────────────────┘
       Moonlight (or any streaming app) streams video and audio as usual.
```

It works next to any streaming host (Vibepollo, Apollo, Sunshine...), which needs no changes.

## Quick start

### 1. PC (Windows)

1. Install [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases). Its driver is signed by Microsoft; no test mode is needed.
2. Download `couchlink-host` from [Releases](../../releases). Before the first release, use the `couchlink-windows-x64` artifact of the latest [CI run](../../actions/workflows/ci.yml), or build it yourself (see below).
3. From an **administrator** terminal, check that Steam recognises the virtual controller:
   ```powershell
   couchlink-host demo 30
   ```
   Open *Steam → Settings → Controller*. A "Steam Controller" should appear with its stick circling and the A button pulsing.
4. Install it, once:
   ```powershell
   couchlink-host install
   ```
   It now starts in the background at every logon, with the firewall set up. Details and troubleshooting: [Windows host setup](docs/host-setup-windows.md).

### 2. iPad / iPhone

Until Couchlink is on the App Store, install `Couchlink-iOS.ipa` from Windows with Sideloadly and a free Apple ID: [Install Couchlink](docs/install-app.md). Then:

1. In Couchlink, tap your PC under **Found on this network** (or enter its address). A 6-digit code pops up on the PC; type it into Couchlink.
2. Switch the controller on (or pair it with **Pair a new controller**).
3. Switch to your streaming app and play.

### Apple TV

tvOS doesn't allow Bluetooth in the background, so Couchlink runs on an iPhone (or iPad) instead: pair the controller with the iPhone, keep it nearby, and stream on the Apple TV as usual. Details: [Apple TV](docs/install-app.md#apple-tv).

## How it works

| Component | What it does |
|---|---|
| [`core/`](core) | Portable C++17, no dependencies. Controller report layouts; Bluetooth → wired report conversion; Steam's identity handshake; the authenticated link protocol (SHA-256/HMAC, sessions, replay protection, PIN pairing); timing statistics. |
| [`host/`](host) | `couchlink-host`: the link server plus a small USB/IP device server that exports a virtual `28DE:1302` Steam Controller. usbip-win2 (Windows) or vhci-hcd (Linux) attaches it as a real USB device. `couchlink-sim` is a stand-in for the iPad, for testing the PC side. |
| [`clients/couchlink-ios/`](clients/couchlink-ios) | The Couchlink app for iPad and iPhone. |
| [`clients/apple-shared/`](clients/apple-shared) | CoreBluetooth driver for Valve's controller protocol, with background state restoration. |

More detail:

- [Architecture](docs/architecture.md)
- [Link protocol](docs/protocol.md)
- [Checking smoothness](docs/timing.md): timing statistics on the device and the PC
- [Research notes](docs/research.md): why this design, and what else exists
- [Security](SECURITY.md)

## Building the PC tools

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This builds on Windows (MSVC), Linux and macOS. Linux hosts work too: load `vhci-hcd`, install `usbip` (linux-tools), and run `couchlink-host` as root.

## Safety

The virtual controller answers Steam's queries itself. It **never forwards firmware updates, factory resets, pairing or calibration commands** to your physical controller. Only settings and haptics reach it. Link traffic is authenticated but not encrypted; see [SECURITY.md](SECURITY.md).

## Credits

This builds on work by the SDL team and Valve (Steam Controller protocol), [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) (the virtual Steam Controller's USB identity), [OpenPuck](https://github.com/safijari/openpuck), and [usbip-win2](https://github.com/vadimgrn/usbip-win2). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

| Part | Licence |
|---|---|
| `core/` (shared protocol and controller code), `clients/apple-shared/`, `clients/couchlink-ios/` (the Couchlink app) | [MIT](core/LICENSE) |
| `host/` (`couchlink-host`) | [GPL-3.0-or-later](LICENSE) |

The app side is MIT so it can be distributed anywhere, including app stores. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the work this builds on.

Not affiliated with or endorsed by Valve Corporation, the Moonlight project or any streaming host project. Steam and Steam Controller are trademarks of Valve Corporation.
