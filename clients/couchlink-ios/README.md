# Couchlink for iPad and iPhone

Couchlink connects your Steam Controller to your gaming PC with full Steam Input, next to **any** streaming app, such as Moonlight.

It reads the controller over Bluetooth and forwards its raw reports to `couchlink-host` on the PC, which plugs in a virtual wired Steam Controller. Couchlink keeps running in the background while you stream.

It runs on iPad and iPhone (iOS 15 or later) and is MIT-licensed. On Apple TV, use an iPhone as the bridge: see [Apple TV](../../docs/install-app.md#apple-tv).

## Status

Background Bluetooth works as well as foreground on iPad. The app also shows how evenly reports arrive, so you can tell Bluetooth from Wi-Fi problems: see [Checking smoothness](../../docs/timing.md).

## Build

CI builds an unsigned `Couchlink-iOS.ipa` on every run; sideload it with Sideloadly (see [Install Couchlink](../../docs/install-app.md)).

With a Mac:

```sh
brew install xcodegen
cd clients/couchlink-ios
xcodegen generate
open Couchlink.xcodeproj
```

## What the files do

| File | Role |
|---|---|
| `Sources/CLKBridge.{h,mm}` | The bridge: Bluetooth controllers, pairing with the PC (Keychain), the authenticated UDP link, haptics back to the controller, timing statistics |
| `Sources/CLKViewController.{h,m}` | The one screen: PC address, pairing prompt, controllers, timing, recent events |
| `../apple-shared/CLKTritonBLE.{h,m}` | CoreBluetooth driver for Valve's controller protocol, with background state restoration |
| `../../core/` | The link protocol and timing statistics, shared with `couchlink-host` |
| `Resources/AppIcon.svg` | The app icon's source; `Resources/Assets.xcassets` holds the 1024 px render |
| `project.yml` | XcodeGen project spec |
