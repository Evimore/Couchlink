# Link protocol (version 1)

UDP, default port **48150** on the PC. Implemented in `core/src/link_protocol.cpp` and `core/src/link_client.cpp`. All integers are little-endian.

## Discovery

`couchlink-host` announces itself over DNS-SD (mDNS) as service type `_couchlink._udp` in `local.`. The instance name is the PC's name, the same `host_name` a ProbeReply carries (dots become dashes); the SRV record gives the link port. The TXT record holds `v=1`, the protocol version. Clients browse for the service, resolve it, and then talk to the address and port as below. On Windows the announcement goes through the system's own mDNS responder (`DnsServiceRegister`, Windows 10 1809 and later).

## Datagram

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"CLK1"` (`0x314B4C43`) |
| 4 | 1 | version (`1`) |
| 5 | 1 | type |
| 6 | 2 | payload length (≤ 160) |
| 8 | 4 | client ID (0 for probes) |
| 12 | 8 | counter |
| 20 | n | payload |
| 20+n | 16 | tag: HMAC-SHA-256(key, bytes 0 … 20+n), truncated. Absent for unauthenticated types. |

A datagram whose length does not match its header is dropped.

## Keys

| Types | Key |
|---|---|
| `Probe`, `ProbeReply`, `PairRequest` | none |
| `Hello`, `HelloAck`, `PairResult` | pairing key (32 random bytes chosen by the client at pairing) |
| everything else | session key = HMAC(pairing key, `"couchlink session v1"` ‖ client nonce ‖ host nonce) |

**Counters**
- **Session messages:** start at 1 per session and direction, and must strictly increase. Stale or duplicate datagrams are dropped, which suits state frames.
- **`Hello`:** the counter is the client's wall clock in microseconds. The host rejects any `Hello` not newer than the last one it accepted from that client.

## Messages

| Type | ID | Direction | Payload |
|---|---|---|---|
| Probe | 0x01 | C→H | nonce u64 |
| ProbeReply | 0x02 | H→C | nonce u64, pairing open u8, host name (u8 length + bytes, ≤ 32) |
| PairRequest | 0x03 | C→H | key [32], nonce u64, client name, proof [16] |
| PairStart | 0x04 | C→H | nonce u64, client name. Answered with a `ProbeReply` carrying the same nonce |
| Hello | 0x10 | C→H | client nonce u64, client name |
| HelloAck | 0x11 | H→C | client nonce u64, host nonce u64, capabilities u32 (bit 0: Steam Controller 2026) |
| PairResult | 0x12 | H→C | accepted u8 |
| Attach | 0x20 | C→H | controller u8, kind u8 (1 = 2026 Steam Controller), transport u8 (1 BLE, 2 USB), reserved u8, VID u16, PID u16, attributes reply [64] (zero = host default), unit serial [20] |
| AttachAck | 0x21 | H→C | controller u8, status u8 (0 ok, 1 unsupported, 2 backend unavailable, 3 failed) |
| Input | 0x22 | C→H | controller u8, length u8, raw HID input report (report ID first, 1–64 bytes) |
| Detach | 0x23 | C→H | controller u8 |
| NeedAttach | 0x24 | H→C | controller u8: input arrived for an unknown controller |
| Ping / Pong | 0x30 / 0x31 | C→H / H→C | client time µs u64 (echoed) |
| HidOutput | 0x40 | H→C | controller u8, kind u8 (1 output report, 2 set feature), length u8, report (ID first) |
| Bye | 0x50 | C→H | — |

## Pairing

1. The client sends `PairStart` once a second until the host answers.
2. If no pairing window is open, the host opens one: it picks a random 6-digit PIN and shows it on the PC's screen, which the user sees through the stream. Its `ProbeReply` says whether pairing is open. `couchlink-host pair` opens a window the same way from the PC; `--no-remote-pairing` makes `PairStart` open nothing.
3. The user types the PIN on the client. The client generates a client ID and a key and sends `PairRequest` once a second. The proof is HMAC(PIN, `"couchlink pair v1"` ‖ client ID ‖ key ‖ nonce), truncated to 16 bytes.
4. The host accepts the first request whose proof matches, stores the client, and answers `PairResult(accepted)`, authenticated with the new key. A wrong PIN gets an authenticated `PairResult(rejected)`.
5. The window closes after 120 s or 5 wrong codes. After 5 wrong codes in a window a `PairStart` opened, `PairStart` is ignored for 5 minutes. For 30 s after success, the host repeats the answer to retransmissions.

While pairing is closed the host does not answer `PairRequest` at all.

## Session lifecycle

- **Connect:** the client sends `Hello` until it gets a `HelloAck` carrying its nonce. The session key is then derived.
- **Attach:** the client sends `Attach` per controller until it gets an `AttachAck`, then streams `Input` for every Bluetooth notification (unreliable, no batching).
- **Keepalive:** the client sends `Ping` every second. The host drops a session after 3 s of silence, releasing every button before unplugging its controllers. The client re-hellos after 4 s without a `Pong`.
- **Roaming:** the host follows the client's source address.
- **Reconnect:** a new `Hello` replaces the session and its controllers.
- **End:** `Detach` and `Bye` end things cleanly.

## Limits

Payloads are authenticated, not encrypted. See [SECURITY.md](../SECURITY.md).
