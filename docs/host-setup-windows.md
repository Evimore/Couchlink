# Windows setup

You need Windows 10 (version 1809 or later) or Windows 11, with Steam installed.

## 1. Install usbip-win2

Download the installer from [usbip-win2 releases](https://github.com/vadimgrn/usbip-win2/releases) and install it. It lets InputLine plug a virtual Steam Controller into Windows. Its drivers are signed by Microsoft, so Secure Boot and anti-cheat stay happy and no test-signing mode is needed.

> HIDMaestro, which uses the same transport, pins usbip-win2 **0.9.7.5** and reports kernel-pool issues in 0.9.7.8. If attaching misbehaves, try another version and please open an issue with the details.

## 2. Install InputLine

Download **`InputLine-Setup-vX.Y.Z.msi`** from [Releases](../../../releases) and run it.

Windows may warn that it "protected your PC", because the installer isn't code-signed yet. Click **More info → Run anyway**.

The installer:

- installs `inputline-host` to `C:\Program Files\InputLine`,
- runs it as the **InputLine** service: it starts with Windows, runs in the background, and restarts by itself if something goes wrong,
- allows UDP port 48150 through the Windows Firewall, from your local network and Tailscale only,
- announces the PC on your local network, so the InputLine app finds it without you typing an address,
- shows the **InputLine icon** in the taskbar's notification area, next to the clock. Its menu shows whether a device is connected, opens the log, and tells you when a newer InputLine is out (the service checks GitHub once a day). *Hide this icon* hides it; open **InputLine** from the Start menu to show it again.

Its files are in `C:\ProgramData\InputLine`: the log (`inputline-host.log`), optional settings (`options.txt`), and your paired devices (in the `pairing` folder, which only administrators can open).

**Updating:** run the newer installer. Paired devices are kept.
**Uninstalling:** *Settings → Apps → Installed apps → InputLine → Uninstall*. Paired devices stay in `C:\ProgramData\InputLine` in case you install again; delete that folder to remove them too.

## 3. Pair your iPad or iPhone

Open InputLine on the iPad or iPhone and tap your PC under **Found on this network** (or enter its address and tap **Connect**). The first time, a Windows notification on the PC shows a 6-digit code; type it into InputLine. (Missed it? Press Windows key + N to see your notifications.) From then on, the device connects by itself.

Not at the PC? Open your streaming app first: the notification is on the PC's screen, so it shows in the stream. Then switch back to InputLine and enter it.

## Checking that Steam sees the controller

If something doesn't work, first check the PC side on its own. Open **Terminal (Admin)** (right-click the Start button) and run:

```powershell
inputline-host demo 30
```

A virtual controller is plugged in for 30 seconds, driven by a test pattern.

1. Open *Steam → Settings → Controller*. You should see a **Steam Controller**.
2. Open its test screen. The left stick circles, A pulses once a second, the right trackpad traces a circle, and gyro sways.

If it never appears, see *Troubleshooting* below.

## Settings

Put extra options in `C:\ProgramData\InputLine\options.txt` (edit it as administrator), then restart the service from **Terminal (Admin)** with `Restart-Service InputLine`. For example:

```
# Log report timing every 10 s (see docs/timing.md)
--stats
```

| Option | What it does |
|---|---|
| `--stats` | Log how evenly controller reports arrive, every 10 s |
| `--verbose` | More detail in the log |
| `--no-remote-pairing` | Don't let devices ask for a pairing code; pair with `inputline-host pair` instead (stop the service first) |
| `--no-discovery` | Don't announce the PC on the local network |
| `--no-update-check` | Don't check GitHub once a day for a newer InputLine |
| `--block-setting N` | Never pass controller setting N from Steam to the physical controller |
| `--name NAME` | The name the app shows for this PC |

`inputline-host --help` lists everything.

Paired devices, from **Terminal (Admin)**:

```powershell
inputline-host clients              # list them
inputline-host forget 1a2b3c4d      # remove one
Restart-Service InputLine           # so the service notices
```

## Without the installer

The `inputline-vX.Y.Z-windows-x64.zip` on the Releases page has the same `inputline-host.exe`, to run by hand. From **Terminal (Admin)** in the unzipped folder:

- `.\inputline-host.exe run` serves the InputLine app until you close the window.
- `.\inputline-host.exe install` sets it up to start at every logon instead of as a service (with the firewall rule), and `.\inputline-host.exe uninstall` undoes that. The installer replaces such a setup by itself.

With `run`, you need a firewall rule yourself:

```powershell
New-NetFirewallRule -DisplayName "InputLine" -Direction Inbound -Protocol UDP -LocalPort 48150 -RemoteAddress LocalSubnet,100.64.0.0/10,fd7a:115c:a1e0::/48 -Profile Any -Action Allow
```

The USB/IP server listens on `127.0.0.1:3240` only and needs no firewall rule.

## Troubleshooting

The log is `C:\ProgramData\InputLine\inputline-host.log` (the one from before the last restart is `inputline-host.log.1`).

| Symptom | Try |
|---|---|
| The installer says it needs usbip-win2 | Install [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases) first (step 1), then run the installer again. |
| `attach: ... failed` in the log | The `attach: usbip said` line just before it gives usbip's reason. Check that `C:\Program Files\USBip\usbip.exe` exists. If usbip-win2 lives elsewhere, add `--usbip-exe "C:\path\to\usbip.exe"` to `options.txt`. |
| InputLine doesn't list the PC | Set the network's profile to **Private** in Windows settings (*Network & internet → your network*): Windows doesn't answer network discovery on Public networks. You can always enter the address instead. |
| InputLine says the PC does not answer | Check that the InputLine service is running (`Get-Service InputLine`) and that the PC's firewall allows it (the installer adds a rule named *InputLine (UDP 48150)*). |
| No pairing notification on the PC | Windows holds notifications back while you play or when *Do not disturb* is on; check the notification centre (Windows key + N). The code is also in the log (`pairing code for ...`). |
| The controller appears but Steam ignores it | Add `--verbose` to `options.txt`, restart the service, and open an issue with the log. |
| The controller disconnects after Steam sends it a setting | The log lists each setting Steam passed on (`Steam set setting N = V`). Setting 49 (wireless protocol version) is never passed on. To test another suspect, add `--block-setting N` to `options.txt`. |
| Windows' USB sound plays when Steam changes a controller config | The physical controller dropped Bluetooth for a moment. The virtual controller stays plugged in for up to 2 minutes, so Steam doesn't notice. The log names each setting Steam passed to the controller just before `lost controller`; please report which one. |
| Buttons stuck after Wi-Fi drops | They shouldn't be: after 3 s of silence the host releases everything. Please report it. |
