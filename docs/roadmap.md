# Roadmap

## Done

- [x] Shared core: report conversion, Steam's identity handshake with a safety filter, link protocol with pairing and replay protection, timing statistics
- [x] `couchlink-host`: USB/IP virtual wired Steam Controller, link server, pairing, `demo` mode
- [x] `couchlink-sim` to test the PC side without an iPad
- [x] End-to-end CI: attach through Linux vhci-hcd, then verify over libusb (descriptors, Steam's handshake, input stream, haptics back to the client, factory reset blocked) and with SDL's Steam Controller driver (two trackpads, gyro, live input)
- [x] Pairing from the couch: the PC shows the code on its screen, the device asks for it
- [x] `couchlink-host install`: background start at logon and firewall setup in one command
- [x] Report the physical controller's firmware version, so Steam doesn't offer an update the virtual controller can't take
- [x] Keep the virtual controller plugged in through Bluetooth or Wi-Fi drops of up to 2 minutes, and replay Steam's settings when the controller returns
- [x] Windows + usbip-win2 + iPad with a real 2026 Steam Controller: Steam shows full Steam Input, all inputs work
- [x] Couchlink app for iPad and iPhone: background Bluetooth with state restoration, Keychain pairing, haptics, timing statistics, event log and shareable report
- [x] Background delivery measured: as smooth as foreground ([Checking smoothness](timing.md))
- [x] Recover single lost datagrams: each one also carries the previous report
- [x] Unsigned `Couchlink-iOS.ipa` on every CI run and release, for sideloading without a Mac
- [x] Automatic discovery: `couchlink-host` announces the PC over DNS-SD (`_couchlink._udp`); Couchlink lists it and follows a paired PC to a new address
- [x] Couchlink remembers every address a PC answered on (home network, VPN) and tries them in turn
- [x] Releases from GitHub's website: Actions → Release → Run workflow
- [x] Windows installer (MSI): `couchlink-host` runs as a service that starts with Windows, with the firewall rule; CI installs and uninstalls it

## Phases

| Phase | What | Status |
|---|---|---|
| 0 | Couchlink app with background Bluetooth; timing measured on the device and at the PC | **Done** |
| 1 | PC program: automatic discovery (done), installer (done; bundling usbip-win2 next), update check, code signing | In progress |
| 2 | Couchlink release: TestFlight, then a free App Store app | Later |
| 3 | More bridges: desktop (Windows, Linux, Mac, Steam Deck; USB, Puck or Bluetooth), Android and Android TV | |
| 4 | Optional: offer the host side to streaming hosts (Vibepollo, Apollo, Sunshine) as a built-in feature | |

## Next

- [ ] Find which setting makes the controller drop Bluetooth when Steam restarts or changes a config, and stop passing it on (setting 49 is already blocked)
- [ ] iPhone with a real 2026 Steam Controller: notification rate and jitter, background with the screen locked
- [ ] Apple TV through an iPhone bridge
- [ ] Settle whether Steam uses the IMU quaternion in report `0x42`. If it does, compute orientation host-side from gyro and accel instead of sending identity.
- [ ] Haptics end to end: trackpad clicks and rumble felt on the controller
- [ ] Measure input latency against Steam Link (240 fps camera, same TV)

## Later

- [ ] Public-key pairing (X25519) so the pairing exchange reveals nothing to a passive observer
- [ ] Optional payload encryption (ChaCha20-Poly1305)
- [ ] Windows service mode and a tray icon for `couchlink-host`
- [ ] 2015 Steam Controller (BLE `0x1106`, wired `28DE:1102` persona)
- [ ] Battery reports over Bluetooth
