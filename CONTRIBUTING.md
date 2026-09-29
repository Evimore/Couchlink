# Contributing

Issues and pull requests are welcome. The most useful contributions right now are reports from real hardware: see *Next* in the [roadmap](docs/roadmap.md).

## Reporting a hardware result

Please include:

- PC OS and version, Steam client version, usbip-win2 version
- iPad / iPhone model and iOS version
- Controller firmware, if you know it
- InputLine's **Share report** and `inputline-host.log`

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

- C++17, no third-party dependencies in `core/` or `host/`.
- New behaviour comes with a test. `host/tests/` has an in-process USB/IP client and a UDP test client for end-to-end tests.
- CI runs the tests under ASan/UBSan and TSan. Please keep both clean.
- The InputLine app builds with XcodeGen (`clients/inputline-ios/project.yml`); CI builds it on every push.

## Releasing

On GitHub: **Actions → Release → Run workflow**, enter the version (for example `0.2.0`) and run it. It builds `inputline-host` for Windows and the unsigned `InputLine-iOS.ipa` from the latest `main`, then publishes them on the **Releases** page under the tag `v0.2.0`, with notes generated from the changes since the last release. A version such as `0.2.0-beta.1` becomes a pre-release. Pushing a `v…` tag does the same.

By contributing you agree that your contribution is licensed under the licence of the part you change: MIT for `core/` and `clients/`, GPL-3.0-or-later for `host/`.
