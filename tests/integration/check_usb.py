#!/usr/bin/env python3
"""Check the virtual Steam Controller at the USB level through the kernel.

Uses libusb (pyusb) so it works even on kernels without the usbhid driver,
such as GitHub's Azure runners. Run as root while couchlink-host has attached
a controller through vhci-hcd and couchlink-sim is streaming.

Checks the HID report descriptor, the feature-report handshake Steam uses,
the stream of state reports, and that a haptic output report written to
the device reaches the client (couchlink-sim logs it).
"""

import sys
import time

import usb.core
import usb.util

VENDOR, PRODUCT = 0x28DE, 0x1302


def feature(dev, request):
    data = bytearray(64)
    data[: len(request)] = request
    dev.ctrl_transfer(0x21, 0x09, 0x0301, 0, bytes(data))  # SET_REPORT (feature 1)
    return bytes(dev.ctrl_transfer(0xA1, 0x01, 0x0301, 0, 64))  # GET_REPORT (feature 1)


def main():
    dev = None
    deadline = time.time() + 30
    while dev is None and time.time() < deadline:
        dev = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
        if dev is None:
            time.sleep(0.5)
    if dev is None:
        print("FAIL: no USB device 28DE:1302")
        return 1
    print(f"found {dev.manufacturer!r} {dev.product!r} on bus {dev.bus} address {dev.address}")
    if (dev.manufacturer, dev.product) != ("Valve Software", "Steam Controller"):
        print("FAIL: unexpected strings")
        return 1

    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
    usb.util.claim_interface(dev, 0)

    descriptor = bytes(dev.ctrl_transfer(0x81, 0x06, 0x2200, 0, 512))
    if len(descriptor) != 372 or descriptor[:4] != b"\x05\x01\x09\x02":
        print(f"FAIL: report descriptor ({len(descriptor)} bytes)")
        return 1
    print("report descriptor ok (372 bytes)")

    reply = feature(dev, b"\x01\x83")
    if reply[1] != 0x83 or reply[2] != 0x19 or reply[4:6] != b"\x02\x13":
        print(f"FAIL: attributes reply {reply[:12].hex()}")
        return 1
    reply = feature(dev, b"\x01\xae\x01\x03")
    if reply[4:16] != b"7054257d2da7":
        print(f"FAIL: string attribute 3 {reply[:24].hex()}")
        return 1
    print("feature handshake ok")

    states, buttons = 0, set()
    deadline = time.time() + 5
    while time.time() < deadline and states < 250:
        report = bytes(dev.read(0x81, 64, timeout=1000))
        if report and report[0] == 0x42:
            if len(report) != 54:
                print(f"FAIL: state report of {len(report)} bytes")
                return 1
            states += 1
            buttons.add(int.from_bytes(report[2:6], "little"))
    print(f"read {states} state reports, {len(buttons)} distinct button states")
    if states < 100 or len(buttons) < 2:
        print("FAIL: expected a steady stream of changing state reports")
        return 1

    # Haptics and settings Steam would send; couchlink-sim logs what arrives.
    dev.write(0x01, bytes([0x81, 0x01, 0x10, 0x00, 0x10, 0x00, 0x03, 0x00]))
    feature(dev, b"\x01\x87\x03\x09\x00\x00")
    feature(dev, b"\x01\x86")  # factory reset: must never reach the client
    usb.util.release_interface(dev, 0)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
