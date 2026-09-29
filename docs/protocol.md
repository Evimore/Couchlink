# Link protocol (version 3)

UDP, default port **48150** on the PC. Implemented in `core/src/link_protocol.cpp` and `core/src/link_client.cpp`. All integers are little-endian.

## Discovery

`inputline-host` announces itself over DNS-SD (mDNS) as service type `_inputline._udp` in `local.`. The instance name is the PC's name, the same `host_name` a ProbeReply carries (dots become dashes); the SRV record gives the link port. The TXT record holds `v=1` (the TXT format version). Clients browse for the service, resolve it, and then talk to the address and port as below. On Windows the announcement goes through the system's own mDNS responder (`DnsServiceRegister`, Windows 10 1809 and later).

## Versions

Byte 4 of every datagram is the protocol version. This document describes version 3; it differs from version 2 in pairing only (version 2 used a plain X25519 exchange with the code mixed into the key).

- **Probe and ProbeReply are understood by every version.** A host answers a Probe whatever its version, and a client reads a ProbeReply whatever its version. Their layouts only ever grow (new fields are appended; readers ignore what they don't know), and a version 1 ProbeReply reads as supporting version 1 only.
- The ProbeReply tells the client the host's supported range (`min`–`max`) and its software version. If the client's version is outside it, the client tells the user which side to update instead of trying to connect.
- Every other message must carry exactly the receiver's version; others are dropped (the host logs which side to update).
- `core/tests/test_compat.cpp` freezes the bytes of every message, an encrypted datagram and the key derivations, and tests every combination of older and newer app and PC.

## Datagram

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"CLK1"` (`0x314B4C43`) |
| 4 | 1 | protocol version (`3`) |
| 5 | 1 | type |
| 6 | 2 | payload length (≤ 160) |
| 8 | 4 | client ID (0 for probes; the attempt's ID for `PairStart` and `PairRequest`) |
| 12 | 8 | counter |
| 20 | n | payload (encrypted for session types) |
| 20+n | 16 | tag (absent for Probe, ProbeReply, PairStart, PairRequest) |

A datagram whose length does not match its header is dropped.

## Protection

| Types | Protection |
|---|---|
| `Probe`, `ProbeReply`, `PairStart`, `PairRequest` | none (no secrets in them) |
| `Hello`, `HelloAck` | HMAC-SHA-256 with the pairing key over bytes 0 … 20+n, truncated to 16 bytes |
| `PairResult` | HMAC-SHA-256 with the result key, or for a wrong code with the failure key (see Pairing) |
| everything else | ChaCha20-Poly1305 (RFC 8439): the header is associated data, the payload is encrypted, the nonce is four zero bytes followed by the counter (little-endian) |

**Session keys:** base = HMAC(pairing key, `"inputline session v2"` ‖ client nonce ‖ host nonce); client→host key = HMAC(base, `"client to host"`); host→client key = HMAC(base, `"host to client"`). One key per direction, so counters never reuse a nonce.

**Counters**
- **Session messages:** start at 1 per session and direction, and must strictly increase. Stale or duplicate datagrams are dropped, which suits state frames.
- **`Hello`:** the counter is the client's wall clock in microseconds. The host rejects any `Hello` not newer than the last one it accepted from that client.

## Messages

| Type | ID | Direction | Payload |
|---|---|---|---|
| Probe | 0x01 | C→H | nonce u64 |
| ProbeReply | 0x02 | H→C | nonce u64, pairing open u8, host name (u8 length + bytes, ≤ 32), min version u8, max version u8, software version (u8 length + bytes, ≤ 32), pairing share [32] (in answer to a `PairStart` while pairing is open: the host's CPace share for that attempt; zero otherwise) |
| PairRequest | 0x03 | C→H | client share [32], nonce u64, client name, proof [16] |
| PairStart | 0x04 | C→H | nonce u64, client name. Answered with a `ProbeReply` carrying the same nonce |
| Hello | 0x10 | C→H | client nonce u64, client name, software version |
| HelloAck | 0x11 | H→C | client nonce u64, host nonce u64, capabilities u32 (bit 0: Steam Controller 2026) |
| PairResult | 0x12 | H→C | accepted u8 |
| Attach | 0x20 | C→H | controller u8, kind u8 (1 = 2026 Steam Controller), transport u8 (1 BLE, 2 USB), flags u8 (see below), VID u16, PID u16, attributes reply [64] (zero = host default), unit serial [20] |
| AttachAck | 0x21 | H→C | controller u8, status u8 (0 ok, 1 unsupported, 2 backend unavailable, 3 failed) |
| Input | 0x22 | C→H | controller u8, length u8, raw HID input report (report ID first, 1–64 bytes) |
| Detach | 0x23 | C→H | controller u8 |
| NeedAttach | 0x24 | H→C | controller u8: input arrived for an unknown controller |
| InputBundle | 0x25 | C→H | controller u8, sequence u32, length u8, report, previous length u8, previous report (the one before, for recovering a lost datagram) |
| Ping / Pong | 0x30 / 0x31 | C→H / H→C | client time µs u64 (echoed) |
| HidOutput | 0x40 | H→C | controller u8, kind u8 (1 output report, 2 set feature), length u8, report (ID first) |
| Bye | 0x50 | C→H | — |

## Pairing

Pairing is **CPace** (draft-irtf-cfrg-cpace), a password-authenticated key exchange, with the cipher suite CPACE-X25519-SHA512 in the initiator-responder setting: the client is the initiator A, the host the responder B, and the code shown on the PC is the password. `core/tests/test_crypto.cpp` checks the implementation against the draft's test vectors.

1. The client picks a random client ID, a random nonce and a random 32-byte secret for this attempt, and sends `PairStart` (header client ID = its ID) once a second until the host answers.
2. If no pairing window is open, the host opens one: it picks a random 6-digit code and shows it on the PC's screen. `inputline-host pair` opens a window the same way from the PC; `--no-remote-pairing` makes `PairStart` open nothing.
3. While a window is open, the host answers each attempt (client ID and nonce) with a `ProbeReply` carrying its CPace share for that attempt: a fresh random secret yb, the generator g (below), and Yb = X25519(yb, g). Retransmitted `PairStart`s get the same share. The host keeps up to 16 attempts per window.
4. The user types the code. Both sides use
   - generator g = CPace `calculate_generator` with PRS = the code (ASCII digits), CI = `"inputline pair v3"` ‖ client ID (u32), sid = the nonce (u64)
   - the client's share Ya = X25519(ya, g), and ISK = SHA-512(lv_cat(`"CPace255_ISK"`, sid, K) ‖ lv_cat(Ya, ADa) ‖ lv_cat(Yb, ADb)) with K = X25519(own secret, other share), ADa = the client's name, ADb = empty. A side aborts if K is all zeros (an invalid or low-order share).
   - pairing key = HMAC(ISK, `"inputline pair v3"` ‖ `"pairing key"`)
   - confirmation, as the draft suggests: mac key = SHA-512(`"CPaceMac"` ‖ sid ‖ ISK); proof = HMAC(mac key, lv_cat(Ya, ADa)) truncated to 16 bytes; result key = HMAC(mac key, lv_cat(Yb, ADb))
5. The client sends `PairRequest` (Ya, the nonce, its name and the proof) once a second. The pairing key never crosses the network.
6. The host uses its secret for that attempt once, then discards it. If the proof matches, it stores the client and its pairing key and answers `PairResult(accepted)` authenticated with the result key, which confirms the host knew the code too. A wrong code gets `PairResult(rejected)` authenticated with the failure key = SHA-256(`"inputline pair v3"` ‖ `"pairing failure"` ‖ client ID ‖ Ya ‖ Yb ‖ nonce), made from public values only (the two sides share no key then). The client then starts a new attempt.
7. The window closes after 120 s or 5 wrong codes. After 5 wrong codes in a window a `PairStart` opened, `PairStart` is ignored for 5 minutes. For 30 s after success, the host repeats the answer to retransmissions.

While pairing is closed the host does not answer `PairRequest` at all. Someone who only listens sees Ya and Yb, which reveal nothing about the code or the keys. Someone who actively takes part in the exchange without knowing the code gets one guess per attempt, checked by the host, and nothing to test further codes offline with; 5 wrong guesses close the window.

## Session lifecycle

- **Connect:** the client sends `Hello` until it gets a `HelloAck` carrying its nonce. The session key is then derived.
- **Attach:** the client sends `Attach` per controller until it gets an `AttachAck`, then streams `Input` for every Bluetooth notification (unreliable, no batching).
- **Keepalive:** the client sends `Ping` every second. The host drops a session after 3 s of silence and releases every button. The client re-hellos after 4 s without a `Pong`.
- **Roaming:** the host follows the client's source address.
- **Controller lost:** when a controller drops off Bluetooth (switched off, out of range), the client sends `Detach` and the host unplugs its virtual controller at once, like a real one. When it comes back it is plugged in afresh and Steam sets it up.
- **Network lost:** when a session ends without `Detach` (silence, or a new `Hello` from the same client), its virtual controllers stay plugged in for 10 s. An `Attach` with flag `0x01` (the controller stayed connected to the client, so it kept Steam's settings) takes over the same virtual controller; without it, the old one is unplugged and a fresh one plugged in.
- **End:** `Bye` unplugs everything at once.

## Limits

See [SECURITY.md](../SECURITY.md) for what the protocol does and doesn't protect.
