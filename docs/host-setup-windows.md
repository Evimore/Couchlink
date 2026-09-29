# Windows setup

You need Windows 10 (version 1809 or later) or Windows 11, with Steam installed.

## 1. Install usbip-win2

Download the installer from [usbip-win2 releases](https://github.com/vadimgrn/usbip-win2/releases) and install it. It lets Couchlink plug a virtual Steam Controller into Windows. Its drivers are signed by Microsoft, so Secure Boot and anti-cheat stay happy and no test-signing mode is needed.

> HIDMaestro, which uses the same transport, pins usbip-win2 **0.9.7.5** and reports kernel-pool issues in 0.9.7.8. If attaching misbehaves, try another version and please open an issue with the details.

## 2. Install Couchlink

Download **`Couchlink-Setup-vX.Y.Z.msi`** from [Releases](../../../releases) and run it.

Windows may warn that it "protected your PC", because the installer isn't code-signed yet. Click **More info → Run anyway**.

The installer:

- installs `couchlink-host` to `C:\Program Files\Couchlink`,
- runs it as the **Couchlink** service: it starts with Windows, runs in the background, and restarts by itself if something goes wrong,
- allows UDP port 48150 through the Windows Firewall, from your local network and Tailscale only,
- announces the PC on your local network, so the Couchlink app finds it without you typing an address.

Its files are in `C:\ProgramData\Couchlink`: the log (`couchlink-host.log`), optional settings (`options.txt`), and your paired devices (in the `pairing` folder, which only administrators can open).

**Updating:** run the newer installer. Paired devices are kept.
**Uninstalling:** *Settings → Apps → Installed apps → Couchlink → Uninstall*. Paired devices stay in `C:\ProgramData\Couchlink` in case you install again; delete that folder to remove them too.

## 3. Pair your iPad or iPhone

Open Couchlink on the iPad or iPhone and tap your PC under **Found on this network** (or enter its address and tap **Connect**). The first time, a small window pops up on the PC with a 6-digit code; type it into Couchlink. From then on, the device connects by itself.

Not at the PC? Open your streaming app first: the code window is on the PC's screen, so it shows in the stream. Then switch back to Couchlink and enter it.

## Checking that Steam sees the controller

If something doesn't work, first check the PC side on its own. Open **Terminal (Admin)** (right-click the Start button) and run:

```powershell
couchlink-host demo 30
```

A virtual controller is plugged in for 30 seconds, driven by a test pattern.

1. Open *Steam → Settings → Controller*. You should see a **Steam Controller**.
2. Open its test screen. The left stick circles, A pulses once a second, the right trackpad traces a circle, and gyro sways.

If it never appears, see *Troubleshooting* below.

## Settings

Put extra options in `C:\ProgramData\Couchlink\options.txt` (edit it as administrator), then restart the service from **Terminal (Admin)** with `Restart-Service Couchlink`. For example:

```
# Log report timing every 10 s (see docs/timing.md)
--stats
```

| Option | What it does |
|---|---|
| `--stats` | Log how evenly controller reports arrive, every 10 s |
| `--verbose` | More detail in the log |
| `--no-remote-pairing` | Don't let devices ask for a pairing code; pair with `couchlink-host pair` instead (stop the service first) |
| `--no-discovery` | Don't announce the PC on the local network |
| `--block-setting N` | Never pass controller setting N from Steam to the physical controller |
| `--name NAME` | The name the app shows for this PC |

`couchlink-host --help` lists everything.

Paired devices, from **Terminal (Admin)**:

```powershell
couchlink-host clients              # list them
couchlink-host forget 1a2b3c4d      # remove one
Restart-Service Couchlink           # so the service notices
```

## Without the installer

The `couchlink-vX.Y.Z-windows-x64.zip` on the Releases page has the same `couchlink-host.exe`, to run by hand. From **Terminal (Admin)** in the unzipped folder:

- `.\couchlink-host.exe run` serves the Couchlink app until you close the window.
- `.\couchlink-host.exe install` sets it up to start at every logon instead of as a service (with the firewall rule), and `.\couchlink-host.exe uninstall` undoes that. The installer replaces such a setup by itself.

With `run`, you need a firewall rule yourself:

```powershell
New-NetFirewallRule -DisplayName "Couchlink" -Direction Inbound -Protocol UDP -LocalPort 48150 -RemoteAddress LocalSubnet,100.64.0.0/10,fd7a:115c:a1e0::/48 -Profile Any -Action Allow
```

The USB/IP server listens on `127.0.0.1:3240` only and needs no firewall rule.

## Troubleshooting

The log is `C:\ProgramData\Couchlink\couchlink-host.log` (the one from before the last restart is `couchlink-host.log.1`).

| Symptom | Try |
|---|---|
| The installer says it needs usbip-win2 | Install [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases) first (step 1), then run the installer again. |
| `attach: ... failed` in the log | Check that `C:\Program Files\USBip\usbip.exe` exists. If usbip-win2 lives elsewhere, add `--usbip-exe "C:\path\to\usbip.exe"` to `options.txt`. |
| Couchlink doesn't list the PC | Set the network's profile to **Private** in Windows settings (*Network & internet → your network*): Windows doesn't answer network discovery on Public networks. You can always enter the address instead. |
| Couchlink says the PC does not answer | Check that the Couchlink service is running (`Get-Service Couchlink`) and that the PC's firewall allows it (the installer adds a rule named *Couchlink (UDP 48150)*). |
| No code window on the PC | A game in exclusive fullscreen can hide it. The code is also in the log (`pairing code for ...`). |
| The controller appears but Steam ignores it | Add `--verbose` to `options.txt`, restart the service, and open an issue with the log. |
| The controller disconnects after Steam sends it a setting | The log lists each setting Steam passed on (`Steam set setting N = V`). Setting 49 (wireless protocol version) is never passed on. To test another suspect, add `--block-setting N` to `options.txt`. |
| Windows' USB sound plays when Steam changes a controller config | The physical controller dropped Bluetooth for a moment. The virtual controller stays plugged in for up to 2 minutes, so Steam doesn't notice. The log names each setting Steam passed to the controller just before `lost controller`; please report which one. |
| Buttons stuck after Wi-Fi drops | They shouldn't be: after 3 s of silence the host releases everything. Please report it. |
