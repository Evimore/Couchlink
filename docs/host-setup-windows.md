# Windows host setup

## 1. Install usbip-win2

Download the installer from [usbip-win2 releases](https://github.com/vadimgrn/usbip-win2/releases) and install it. Its drivers are signed by Microsoft, so Secure Boot and anti-cheat stay happy and no test-signing mode is needed. It installs to `C:\Program Files\USBip`, where `couchlink-host` finds it automatically.

> HIDMaestro, which uses the same transport, pins usbip-win2 **0.9.7.5** and reports kernel-pool issues in 0.9.7.8. If attaching misbehaves, try another version and please open an issue with the details.

## 2. Check the PC side on its own

Open **Terminal (Admin)** in the folder with `couchlink-host.exe`:

```powershell
.\couchlink-host.exe demo 60
```

A virtual controller is plugged in for 60 seconds.

1. Open *Steam → Settings → Controller*. You should see a **Steam Controller**.
2. Open its test screen. The left stick circles, A pulses once a second, the right trackpad traces a circle, and gyro sways.
3. Output such as `demo: Steam sent setting 0x87` means Steam is configuring the controller. That's a good sign.

If the controller never appears, see *Troubleshooting* below.

## 3. Install it

From **Terminal (Admin)**, in the folder with `couchlink-host.exe`:

```powershell
.\couchlink-host.exe install
```

This is the only command you need. It:

- copies `couchlink-host.exe` to `C:\Program Files\Couchlink`,
- allows UDP 48150 through the Windows Firewall, for your local network and Tailscale only,
- starts `couchlink-host` in the background now and at every logon, with the administrator rights `usbip attach` needs,
- announces the PC on your local network, so Couchlink lists it without you typing an address. Add `--no-discovery` to turn that off.

The log goes to `%LOCALAPPDATA%\Couchlink\couchlink-host.log`. To remove it all, run `couchlink-host uninstall`. Paired devices are kept.

## 4. Pair your iPad or iPhone

Open Couchlink on the iPad or iPhone and tap your PC under **Found on this network** (or enter its address and tap **Connect**). The first time, a small window pops up on the PC with a 6-digit code; type it into Couchlink. That's it: from then on the device connects by itself.

Not at the PC? Open your streaming app first: the code window is on the PC's screen, so it shows in the stream. Then switch back to Couchlink and enter it.

Nothing needs typing on the PC. If you'd rather not allow pairing requests from the network, install with `--no-remote-pairing`; you then run `couchlink-host pair` on the PC when you want to add a device.

Other commands:

```powershell
.\couchlink-host.exe clients        # list paired devices
.\couchlink-host.exe forget 1a2b3c4d
.\couchlink-host.exe --help
```

`clients` and `forget` edit the pairing file. After `forget`, restart the background copy (sign out and in, or `couchlink-host install` again) so it takes effect.

### Without installing

`couchlink-host run` does the same in a terminal window, and `couchlink-host pair` shows a pairing code right away. Both need an administrator terminal. You then need a firewall rule yourself:

```powershell
New-NetFirewallRule -DisplayName "Couchlink" -Direction Inbound -Protocol UDP -LocalPort 48150 -RemoteAddress LocalSubnet,100.64.0.0/10,fd7a:115c:a1e0::/48 -Profile Any -Action Allow
```

The USB/IP server listens on `127.0.0.1:3240` only and needs no firewall rule.

## Troubleshooting

| Symptom | Try |
|---|---|
| `attach: ... failed` | Run the terminal as administrator. Check that `C:\Program Files\USBip\usbip.exe` exists, or pass `--usbip-exe`. |
| Port 3240 or 48150 in use | `couchlink-host` is probably already running in the background (installed). Otherwise another USB/IP server is running: use `--usbip-port 3241`. |
| Couchlink doesn't list the PC | Set the network's profile to **Private** in Windows settings: Windows doesn't answer network discovery on Public networks. Discovery also needs Windows 10 1809 or later. You can always enter the address instead. |
| Couchlink says the PC does not answer | Check that `couchlink-host` is running (Task Manager → Details → `couchlink-host.exe`) and the firewall rule exists. `couchlink-sim --host <PC IP> probe`, run from another machine, should answer. |
| No code window on the PC | A game in exclusive fullscreen can hide it; the code is also in the log. Or run `couchlink-host uninstall`, then `couchlink-host pair` in a terminal. |
| The controller appears but Steam ignores it | Run with `--verbose` and open an issue with the log. Steam's claim handshake may have changed. |
| The controller disconnects after Steam sends it a setting | The log lists each setting Steam passed on (`Steam set setting N = V`). Setting 49 (wireless protocol version) is never passed on. To test another suspect, reinstall with `couchlink-host install --block-setting N`. |
| Windows' USB sound plays when Steam changes a controller config | The physical controller dropped Bluetooth for a moment. The virtual controller stays plugged in for up to 2 minutes, so Steam doesn't notice. The log names each setting Steam passed to the controller (`Steam set setting ...`) just before `lost controller`; please report which one. |
| Buttons stuck after Wi-Fi drops | They shouldn't be: after 3 s of silence the host releases everything. Please report it. |

Logs go to the console. Add `--verbose` for USB-level detail.
