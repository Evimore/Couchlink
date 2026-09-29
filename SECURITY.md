# Security

## What the link protects

`inputline-host` accepts controller input over UDP from paired devices only.

- **Pairing:** the PC shows a random 6-digit code on its own screen; you type it on the device. The device and the PC run an X25519 key exchange, and both derive the pairing key from its result and the code: the key itself never crosses the network, and someone who only listens learns nothing to guess the code with. A pairing window lasts 2 minutes and closes after 5 wrong codes; requests started from the network are then ignored for 5 minutes. `--no-remote-pairing` limits pairing to `inputline-host pair`.
- **Every session** derives fresh keys, one per direction, from the pairing key and two random nonces.
- **Every session datagram is encrypted and authenticated** with ChaCha20-Poly1305 (RFC 8439) and carries a strictly increasing counter, so controller input and haptics can't be read, forged, altered or replayed. The same class of protection streaming apps such as Moonlight and Steam Link use.
- The cryptography (X25519, ChaCha20-Poly1305, SHA-256) is small, dependency-free code in `core/`, tested against the RFC test vectors and an independent implementation.
- **Pairing keys** are stored in the iOS Keychain (this device only) and, on the PC, in a config file readable only by your account on Linux. On Windows it lives in `C:\ProgramData\InputLine\pairing`, which only administrators and Windows itself can open.

## What it does not do

- **Pairing is protected against listening, not against an attacker in the middle.** Someone who can actively intercept and alter traffic between the device and the PC during the 2-minute pairing window could take part in the exchange and try the code offline. That needs control of your network at exactly that moment; pair at home. A password-authenticated key exchange (PAKE) would close this gap and is on the roadmap.
- **Some metadata stays visible:** that a device talks to the PC, how often, and the device's name in `Hello` and pairing messages.
- **Discovery announces the PC's name** and link port on the local network (DNS-SD `_inputline._udp`), like any AirPlay or printer service. `--no-discovery` turns it off.
- **`inputline-host` needs administrator/root rights** to run `usbip attach`; the Windows installer runs it as a service (LocalSystem). Its USB/IP server listens on 127.0.0.1 only. The service reads extra options from `C:\ProgramData\InputLine\options.txt`, a folder only administrators can change.
- **Update check:** once a day the service asks GitHub (`api.github.com`) for InputLine's public list of releases, to tell you when a newer version is out. Nothing about your PC or devices is sent; GitHub sees the request like any web visit. `--no-update-check` in `options.txt` turns it off.
- **The code that reads input from the network or other programs** (the link protocol, the link server, the local USB/IP server and the update check) is fuzzed in CI.
- **Firmware updates, factory reset, and serial, pairing and radio writes from the PC are never forwarded** to your physical controller (see `core/src/feature_responder.cpp`). Steam cannot update the real controller's firmware or change its identity through the link; plug it into the PC for firmware updates. Settings, haptics, turning it off and calibration do reach it.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting ("Report a vulnerability" under the repository's *Security* tab) rather than a public issue.
