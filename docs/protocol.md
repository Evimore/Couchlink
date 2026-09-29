# Link protocol (version 2)

UDP, default port **48150** on the PC. Implemented in `core/src/link_protocol.cpp` and `core/src/link_client.cpp`. All integers are little-endian.

## Discovery

`couchlink-host` announces itself over DNS-SD (mDNS) as service type `_couchlink._udp` in `local.`. The instance name is the PC's name, the same `host_name` a ProbeReply carries (dots become dashes); the SRV record gives the link port. The TXT record holds `v=1` (the TXT format version). Clients browse for the service, resolve it, and then talk to the address and port as below. On Windows the announcement goes through the system's own mDNS responder (`DnsServiceRegister`, Windows 10 1809 and later).

## Versions

Byte 4 of every datagram is the protocol version. This document describes version 2.

- **Probe and ProbeReply are understood by every version.** A host answers a Probe whatever its version, and a client reads a ProbeReply whatever its version. Their layouts only ever grow (new fields are appended; readers ignore what they don't know), and a version 1 ProbeReply reads as supporting version 1 only.
- The ProbeReply tells the client the host's supported range (`min`–`max`) and its software version. If the client's version is outside it, the client tells the user which side to update instead of trying to connect.
- Every other message must carry exactly the receiver's version; others are dropped (the host logs which side to update).
- `core/tests/test_compat.cpp` freezes the bytes of every message, an encrypted datagram and the key derivations, and tests every combination of older and newer app and PC.

## Datagram

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"CLK1"` (`0x314B4C43`) |
| 4 | 1 | protocol version (`2`) |
| 5 | 1 | type |
| 6 | 2 | payload length (≤ 160) |
| 8 | 4 | client ID (0 for probes) |
| 12 | 8 | counter |
| 20 | n | payload (encrypted for session types) |
| 20+n | 16 | tag (absent for Probe, ProbeReply, PairStart, PairRequest) |

A datagram whose length does not match its header is dropped.

## Protection

| Types | Protection |
|---|---|
| `Probe`, `ProbeReply`, `PairStart`, `PairRequest` | none (no secrets in them) |
| `Hello`, `HelloAck` | HMAC-SHA-256 with the pairing key over bytes 0 … 20+n, truncated to 16 bytes |
| `PairResult` | HMAC-SHA-256 with the pairing result key (see Pairing) |
| everything else | ChaCha20-Poly1305 (RFC 8439): the header is associated data, the payload is encrypted, the nonce is four zero bytes followed by the counter (little-endian) |

**Session keys:** base = HMAC(pairing key, `"couchlink session v2"` ‖ client nonce ‖ host nonce); client→host key = HMAC(base, `"client to host"`); host→client key = HMAC(base, `"host to client"`). One key per direction, so counters never reuse a nonce.

**Counters**
- **Session messages:** start at 1 per session and direction, and must strictly increase. Stale or duplicate datagrams are dropped, which suits state frames.
- **`Hello`:** the counter is the client's wall clock in microseconds. The host rejects any `Hello` not newer than the last one it accepted from that client.

## Messages

| Type | ID | Direction | Payload |
|---|---|---|---|
| Probe | 0x01 | C→H | nonce u64 |
| ProbeReply | 0x02 | H→C | nonce u64, pairing open u8, host name (u8 length + bytes, ≤ 32), min version u8, max version u8, software version (u8 length + bytes, ≤ 32), pairing public key [32] (zero unless pairing is open) |
| PairRequest | 0x03 | C→H | client public key [32], nonce u64, client name, proof [16] |
| PairStart | 0x04 | C→H | nonce u64, client name. Answered with a `ProbeReply` carrying the same nonce |
| Hello | 0x10 | C→H | client nonce u64, client name, software version |
| HelloAck | 0x11 | H→C | client nonce u64, host nonce u64, capabilities u32 (bit 0: Steam Controller 2026) |
| PairResult | 0x12 | H→C | accepted u8 |
| Attach | 0x20 | C→H | controller u8, kind u8 (1 = 2026 Steam Controller), transport u8 (1 BLE, 2 USB), reserved u8, VID u16, PID u16, attributes reply [64] (zero = host default), unit serial [20] |
| AttachAck | 0x21 | H→C | controller u8, status u8 (0 ok, 1 unsupported, 2 backend unavailable, 3 failed) |
| Input | 0x22 | C→H | controller u8, length u8, raw HID input report (report ID first, 1–64 bytes) |
| Detach | 0x23 | C→H | controller u8 |
| NeedAttach | 0x24 | H→C | controller u8: input arrived for an unknown controller |
| InputBundle | 0x25 | C→H | controller u8, sequence u32, length u8, report, previous length u8, previous report (the one before, for recovering a lost datagram) |
| Ping / Pong | 0x30 / 0x31 | C→H / H→C | client time µs u64 (echoed) |
| HidOutput | 0x40 | H→C | controller u8, kind u8 (1 output report, 2 set feature), length u8, report (ID first) |
| Bye | 0x50 | C→H | — |

## Pairing

1. The client makes a temporary X25519 key pair and sends `PairStart` once a second until the host answers.
2. If no pairing window is open, the host opens one: it makes its own temporary X25519 key pair, picks a random 6-digit code and shows it on the PC's screen. Its `ProbeReply` says pairing is open and carries the host's public key. `couchlink-host pair` opens a window the same way from the PC; `--no-remote-pairing` makes `PairStart` open nothing.
3. The user types the code on the client. Both sides compute the X25519 shared secret Z and the transcript T = `"couchlink pair v2"` ‖ client ID ‖ client public key ‖ host public key ‖ nonce, then:
   - pairing key = HMAC(Z, T ‖ `"pairing key"` ‖ code)
   - result key = HMAC(Z, T ‖ `"pairing result"`)
   - proof = HMAC(pairing key, T ‖ `"pairing proof"`), truncated to 16 bytes
4. The client sends `PairRequest` (its public key, the nonce, its name and the proof) once a second. The pairing key itself never crosses the network.
5. The host accepts the first request whose proof matches, stores the client and its pairing key, and answers `PairResult(accepted)` authenticated with the result key. A wrong code gets `PairResult(rejected)`, also authenticated with the result key, which does not depend on the code.
6. The window closes after 120 s or 5 wrong codes. After 5 wrong codes in a window a `PairStart` opened, `PairStart` is ignored for 5 minutes. For 30 s after success, the host repeats the answer to retransmissions.

While pairing is closed the host does not answer `PairRequest` at all. Someone who only listens sees the public keys but not Z, so learns neither the pairing key nor anything to guess the code with.

## Session lifecycle

- **Connect:** the client sends `Hello` until it gets a `HelloAck` carrying its nonce. The session key is then derived.
- **Attach:** the client sends `Attach` per controller until it gets an `AttachAck`, then streams `Input` for every Bluetooth notification (unreliable, no batching).
- **Keepalive:** the client sends `Ping` every second. The host drops a session after 3 s of silence, releasing every button before unplugging its controllers. The client re-hellos after 4 s without a `Pong`.
- **Roaming:** the host follows the client's source address.
- **Reconnect:** a new `Hello` replaces the session and its controllers.
- **End:** `Detach` and `Bye` end things cleanly.

## Limits

An attacker who actively sits between the device and the PC during the 2-minute pairing window could try to take part in the exchange; see [SECURITY.md](../SECURITY.md).
