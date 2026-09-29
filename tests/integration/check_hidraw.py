#!/usr/bin/env python3
"""Check the virtual Steam Controller through the Linux HID stack.

Run as root after inputline-host attached a controller through vhci-hcd. Finds
the hidraw node for 28DE:1302, reads state reports, and performs the same
feature-report handshake Steam uses to identify the controller.
"""

import fcntl
import glob
import os
import sys
import time

VENDOR, PRODUCT = 0x28DE, 0x1302


def ioc(direction, number, size):
    return (direction << 30) | (size << 16) | (ord("H") << 8) | number


def hidiocsfeature(size):
    return ioc(3, 0x06, size)


def hidiocgfeature(size):
    return ioc(3, 0x07, size)


def find_device(timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for uevent in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
            with open(uevent) as f:
                text = f.read()
            if f"HID_ID=0003:{VENDOR:08X}:{PRODUCT:08X}" in text:
                return "/dev/" + uevent.split("/")[4], text
        time.sleep(0.5)
    return None, None


def main():
    node, uevent = find_device(30)
    if node is None:
        print("FAIL: no hidraw device for 28DE:1302")
        return 1
    name = [line for line in uevent.splitlines() if line.startswith("HID_NAME=")]
    print(f"found {node} {name}")

    fd = os.open(node, os.O_RDWR)
    try:
        states = 0
        buttons_seen = set()
        deadline = time.time() + 5
        while time.time() < deadline and states < 200:
            report = os.read(fd, 64)
            if report and report[0] == 0x42:
                if len(report) != 54:
                    print(f"FAIL: state report has {len(report)} bytes")
                    return 1
                states += 1
                buttons_seen.add(int.from_bytes(report[2:6], "little"))
        print(f"read {states} state reports; distinct button states: {len(buttons_seen)}")
        if states < 50 or len(buttons_seen) < 2:
            print("FAIL: expected a stream of changing state reports")
            return 1

        # GET_ATTRIBUTES_VALUES, the first thing Steam asks.
        request = bytearray(64)
        request[0], request[1] = 0x01, 0x83
        fcntl.ioctl(fd, hidiocsfeature(len(request)), bytes(request))
        reply = bytearray(64)
        reply[0] = 0x01
        fcntl.ioctl(fd, hidiocgfeature(len(reply)), reply)
        if reply[1] != 0x83 or reply[2] != 0x19 or reply[4:6] != b"\x02\x13":
            print(f"FAIL: unexpected attributes reply {reply[:12].hex()}")
            return 1
        print(f"attributes reply ok: {reply[:28].hex()}")

        # GET_STRING_ATTRIBUTE index 3: the Valve constant.
        request = bytearray(64)
        request[0], request[1], request[2], request[3] = 0x01, 0xAE, 0x01, 0x03
        fcntl.ioctl(fd, hidiocsfeature(len(request)), bytes(request))
        reply = bytearray(64)
        reply[0] = 0x01
        fcntl.ioctl(fd, hidiocgfeature(len(reply)), reply)
        text = bytes(reply[4:24]).split(b"\0")[0].decode()
        if text != "7054257d2da7":
            print(f"FAIL: string attribute 3 was {text!r}")
            return 1
        print("string attribute ok")
    finally:
        os.close(fd)

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
