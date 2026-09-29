# Checking smoothness

Couchlink measures how evenly controller reports arrive at both ends of the link. If input feels laggy or stutters, these numbers show whether Bluetooth or the network is the cause.

- **On the iPad or iPhone:** Couchlink shows how evenly reports arrive over Bluetooth, separately for **Foreground** (Couchlink in front) and **Background** (another app in front).
- **On the PC:** `couchlink-host --stats` logs how evenly they arrive over the network, every 10 s.

## What to expect

- **About 67 reports per second over Bluetooth.** iOS and iPadOS take a report from the controller every 15 ms. That's their Bluetooth connection interval, and it applies to every app, Steam Link included. Over USB the controller reports every 4 ms.
- **The same in the background.** iOS delivers the controller's reports to Couchlink in the background as evenly as in the foreground.
- **The network is the usual suspect.** Stutter that shows up at the PC but not on the device comes from the network in between; a busy Wi-Fi network makes the streaming app's video stutter too. Couchlink makes up for a single lost datagram, since each one also carries the previous report.

## Measure it yourself

1. On the PC, install with statistics on (from **Terminal (Admin)**):
   ```powershell
   couchlink-host install --stats
   ```
   The `stats:` lines go to `%LOCALAPPDATA%\Couchlink\couchlink-host.log`.
2. In Couchlink, tap **Reset timing**, then use the controller for about 2 minutes with Couchlink in front: sticks, both trackpads, a bit of gyro. The **Foreground** line fills in.
3. Switch to your streaming app and play for at least 10 minutes.
4. Switch back to Couchlink and tap **Share report**. Compare it with the `stats:` lines from the PC's log.

Tips:

- Put the device on **5 GHz or 6 GHz Wi-Fi** if you can. On 2.4 GHz, Wi-Fi and Bluetooth share the radio.
- If you connect from outside your home network, try once on the local network too, to see how much the path adds.

## How to read the numbers

Each line looks like:

```
67 reports/s, gap p50 14.8 ms, p95 16.1 ms, p99 29 ms, max 45 ms, >20 ms: 31, >50 ms: 0, >100 ms: 0
```

| Value | Meaning | Good |
|---|---|---|
| reports/s | How many reports arrived per second | About 67 over Bluetooth on iOS, the same in background as in foreground, and the same at the PC |
| gap p50 / p95 / p99 | Time between reports: typical, 1-in-20, 1-in-100 | p99 within a few reports of p50 |
| max, >20/50/100 ms | Long hiccups, felt as stutter or stuck trackpads | Rare |

The PC's line also counts lost datagrams and how many lost reports were recovered.

- **Device fine, PC not:** the network between them. Try another Wi-Fi band, move closer to the router, or use Ethernet on the PC.
- **Background clearly worse than foreground:** please open an issue with the report and your iOS version.
