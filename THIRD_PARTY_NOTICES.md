# Third-party notices

`host/` is licensed under the GNU General Public License v3.0 or later (see `LICENSE`); `core/`, `clients/apple-shared/` and `clients/inputline-ios/` under the MIT licence (see `core/LICENSE`). They build on published work from the projects below.

## HIDMaestro (MIT)

`host/src/triton_descriptors.h` and the default identity values in `core/src/feature_responder.cpp` come from HIDMaestro's `steam-controller-2` profile (https://github.com/hifihedgehog/HIDMaestro). That profile records the USB descriptors and feature-report answers of a real 2026 Steam Controller.

```
Copyright (c) 2026 HIDMaestro Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## OpenPuck (AGPL-3.0)

The HID report descriptor in `host/src/triton_descriptors.h` was first captured by OpenPuck's ReversePuckFirmware (https://github.com/safijari/openpuck), via HIDMaestro. It describes the interface of Valve's hardware and is reproduced byte for byte because host software identifies the controller by it.

## Simple DirectMedia Layer (zlib)

The report layouts in `core/include/inputline/triton.h`, the Bluetooth GATT layout used by `clients/apple-shared/ILNTritonBLE.m`, and the setting and command IDs mirror SDL's Valve-authored Steam Controller code (`src/joystick/hidapi/steam/controller_structs.h`, `controller_constants.h`, `SDL_hidapi_steam_triton.c`, `src/hidapi/ios/hid.m`). No SDL source file is copied verbatim.

```
Copyright (C) 2020 Valve Corporation
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## usbip-win2 (BSD-2-Clause)

https://github.com/vadimgrn/usbip-win2 — the Windows USB/IP client that plugs the virtual controller into Windows. `InputLine-Setup-vX.exe` includes its unmodified installer (version 0.9.8.1, checked by SHA-256 in `installer/windows/get-usbip-win2.ps1`) and runs it when usbip-win2 isn't installed yet. InputLine's other downloads don't include it.

```
BSD 2-Clause License

Copyright (c) 2021-2026, Vadym Hrynchyshyn <vadimgrn@gmail.com>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Trademarks

Steam, the Steam logo and Steam Controller are trademarks of Valve Corporation. This project is not affiliated with, endorsed by, or sponsored by Valve, the Moonlight project, or any streaming host project.
