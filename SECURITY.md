# Security

## What the link protects

`couchlink-host` accepts controller input over UDP from paired devices only.

- **Pairing:** the PC shows a random 6-digit code on its own screen; you type it on the device. The device proves it knows the code with HMAC-SHA-256 before the PC stores its key. A pairing window lasts 2 minutes and closes after 5 wrong codes; requests started from the network are then ignored for 5 minutes. `--no-remote-pairing` limits pairing to `couchlink-host pair`.
- **Every session** derives a fresh key from the pairing key and two random nonces.
- **Every datagram** carries a 128-bit truncated HMAC-SHA-256 tag and a strictly increasing counter, so datagrams cannot be forged, altered or replayed.
- **Pairing keys** are stored in the iOS Keychain (this device only) and, on the PC, in a config file readable only by your account on Linux. On Windows it lives in `C:\ProgramData\Couchlink\pairing`, which only administrators and Windows itself can open.

## What it does not do

- **Payloads are not encrypted.** Someone on your network can observe controller input (buttons, sticks, motion) and haptic commands. They cannot inject input.
- **The pairing exchange sends the new key in the clear**, once. Someone recording your network during those seconds could later authenticate as that device. Pair on a network you trust. A key exchange with public-key cryptography is on the roadmap.
- **Discovery announces the PC's name** and link port on the local network (DNS-SD `_couchlink._udp`), like any AirPlay or printer service. `--no-discovery` turns it off.
- **`couchlink-host` needs administrator/root rights** to run `usbip attach`; the Windows installer runs it as a service (LocalSystem). Its USB/IP server listens on 127.0.0.1 only. The service reads extra options from `C:\ProgramData\Couchlink\options.txt`, a folder only administrators can change.
- **Firmware, flash, pairing and calibration commands from the PC are never forwarded** to your physical controller (see `core/src/feature_responder.cpp`). Steam cannot update or reconfigure the real controller through the link; plug it into the PC for firmware updates.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting ("Report a vulnerability" under the repository's *Security* tab) rather than a public issue.
