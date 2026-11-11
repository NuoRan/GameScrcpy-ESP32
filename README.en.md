# ESP32 touch box · a GameScrcpy community extension

[简体中文](README.md)

Turns an ESP32 development board into the phone's plugged-in touch screen, keyboard, mouse and gamepad: the board is on the phone's USB port or Bluetooth, and [GameScrcpy](https://github.com/NuoRan/GameScrcpy) on the computer sends it the touches over a serial port, Wi-Fi or Bluetooth, so the phone needs no USB debugging. This repository is the GameScrcpy community extension `community.esp32` by "GameScrcpy 社区", released under Apache-2.0. It used to be the ESP32 touch box built into GameScrcpy 2.0 and now uses only GameScrcpy's public extension interfaces.

## Requirements

- GameScrcpy 2.1 or later on Windows 10 / 11 (x64), for extension ABI 2: jobs, device providers, events, settings, hardware claims, panels in a device's and the extension's settings, and extension help.
- An ESP32-S3, S2 or P4 (USB to the phone) or a classic ESP32, C3 or C6 (Bluetooth to the phone) board with at least 4 MB of flash.
- For the computer to reach the board over Bluetooth: Bluetooth LE on the computer (built in or a USB adapter), and a classic ESP32, C3, C6 or S3 with firmware 1.1 or later.

## Install and use

1. Download `community.esp32-<version>.gsext`, choose Install from file on GameScrcpy's Extensions page and allow what it asks for: the devices you choose (serial ports, Bluetooth and flashing), adding devices to the Devices page, and the network (only to reach boards over Wi-Fi on your local network).
2. Plug the board's serial port into the computer. Once found, it shows up on the Devices page as a touch box: Control works the phone without a picture, Settings opens the board's window (Try it, firmware, Wi-Fi, Bluetooth, name, diagnosis), and More holds Flash firmware, Diagnose and Disconnect.
3. To touch a mirrored phone through the board, choose the touch method ESP32 in that phone's settings, and the serial port (auto or a port such as COM7), Wi-Fi or Bluetooth, with the address, in this extension's settings for that phone.
4. For a new board or one with the old firmware, pick its serial port in the extension card's Settings and click Flash firmware.
5. For the computer to reach the board over Bluetooth without a cable: while the board is on the serial port, click Pair over Bluetooth in its window, then choose Bluetooth as the connection. A board on Bluetooth shows a Bluetooth icon and its signal on the Devices page; when the slowest round trip in every hundred goes over 30 ms its window says "Bluetooth delay is high".

The details (wiring, when to hold BOOT, what the messages mean) are in the Extensions group of GameScrcpy's Help page; the source is [docs/help/esp32.md](docs/help/esp32.md).

### The built-in ESP32 of GameScrcpy

- This extension reads nothing of the built-in `gamescrcpy.esp32`: its touch method and `esp32.*` settings are not carried over, and a board's Wi-Fi has to be set up once more here.
- With both on they fight over the same serial port; turn the built-in ESP32 touch box off on the Extensions page while you use this one.
- When it installs an extension, GameScrcpy checks the system functions the DLL uses: this DLL opens serial ports with `CreateFile` and reads its own firmware images, so the install dialog may say its code reads and writes files. It touches no other files.

## Layout

| Folder | What it holds |
|---|---|
| `extension/` | The manifest `manifest.json`, the script that draws the panels `scripts/panel.ts`, the icon |
| `native/support` | The little that was taken from GameScrcpy's `gs::base` / `gs::platform`: `Expected`, logging and the Win32 helpers for serial ports and UDP |
| `native/esp32` | The link to the board: serial port, UDP, Bluetooth LE (WinRT GATT), mDNS discovery, contact slots, keys, diagnosis (GameScrcpy's `src/esp32`) |
| `native/espflash` | Firmware flashing over Espressif's public serial download protocol (GameScrcpy's `src/espflash`) |
| `native/touchbox` | The extension itself: finding boards, the touch method, the device provider, jobs, nodes, events, and `esp32.dll` that exports `gs_ext_main` |
| `firmware/` | The ESP-IDF v6.1 firmware project and its built images in `release/` (GameScrcpy's `firmware/esp32`) |
| `protocol/` | The computer ↔ board protocol: `SPEC.md`, the shared C header, the vector generator and the golden vectors |
| `sdk/include/gamescrcpy_ext.h` | The C header of GameScrcpy's native extension interface, taken from GameScrcpy commit `6a26cb0e` (2.1.0 in development, ABI 2); replace it with the one in `sdk/include/` of the GameScrcpy release you build against |
| `docs/help/` | The help article the extension brings (English and Simplified Chinese) |
| `tools/` | `pack.ps1` (makes the package), `build_firmware.ps1` (builds the firmware), `fake_esp32.py` (a fake board on Wi-Fi) |
| `tests/unit` | doctest unit tests: protocol vectors, link and handshake, flashing, finding boards, the extension's C interface |
| `tests/extension` | Scenarios for `gs ext test` |
| `third_party/` | doctest, tl::expected, nlohmann/json, miniz (versions and checksums in [third_party/README.md](third_party/README.md)) |

## Build and test

You need Visual Studio 2022 (MSVC x64), CMake 3.28+ and Ninja; in a Developer PowerShell for VS 2022:

```powershell
cmake --workflow --preset msvc-debug
ctest --preset msvc-debug
```

The presets are `msvc-debug`, `msvc-release` (LTO), `msvc-asan` and `ci` (warnings as errors). Outputs land in `build/<preset>/`: `package/` holds what goes into the extension package (DLL, manifest, script, icon, help, firmware, license texts) and `bin/` the test programs. The code follows GameScrcpy's rules: `/W4 /WX /permissive-`, C++20, no comments, no fixed delays for synchronization.

## Package

```powershell
$env:GS_HOME = 'C:\Program Files\GameScrcpy'
.\tools\pack.ps1
```

`pack.ps1` builds `esp32.dll` with the `msvc-release` preset, checks the package's files and the SHA-256 of the firmware images, and writes `dist/community.esp32-<version>.gsext` with `gs.exe pack` from `GS_HOME`. The version lives only in `extension/manifest.json`.

## Scenarios for `gs ext test`

```powershell
$tests = Get-ChildItem tests\extension\*.json | ForEach-Object { '--test'; $_.FullName }
& "$env:GS_HOME\gs.exe" ext test build\msvc-release\package @tests
```

The package carries no tests, so each scenario goes to `gs ext test` with its own `--test`. The scenarios need no board and never make the computer's Bluetooth search; they set the serial port to `COM250`, which is not there, or the connection to Bluetooth, so what is plugged into the computer does not change the outcome. The format is GameScrcpy's `gamescrcpy.ext-test` version 1: one JSON file per scenario with optional `settings`, `deviceSettings` (the phone is `mock-phone`), `holders` and `timeout`; each of its `steps` names its action in `do` (`call`, `job`, `touch.open`, `devices`, `device.act`, `settings`, `holder`, `claim.want`, `claims` and more) and what to check in `expect`. Every scenario starts with an empty store, which cannot hold a board's key, so the old scenario with a fake board on Wi-Fi (`tools/fake_esp32.py`) is gone; the unit tests in `tests/unit` cover the links with fake boards.

## The interface for logic and panels

The extension's script, its panels and your own logic can use `ext.community.esp32`:

| Name | Kind | What it does |
|---|---|---|
| `boards()` | node → json | The boards found: id, state, connection, chip, firmware, phone, Wi-Fi, round trip, Bluetooth signal and slowest round trip, whether an upgrade is there |
| `bluetooth()` | node → json | The computer's Bluetooth state, the boards seen nearby lately (address, chip, signal, whether this computer has its key, whether it is paired or connected) and the boards paired over Bluetooth |
| `ports()` | node → json | The computer's serial ports: name, title, kind, whether it looks like a board |
| `firmware()` | node → json | The bundled firmware: version and the chips it has images for |
| `problem()` | node → string | Why the last search found no board |
| `language()` | node → string | The language of the extension's texts (`zh_CN` or `en`, following the Windows display language) |
| `tap(device)`, `disconnect(device)`, `rescan()` | node → boolean | Try it, let go of a board, look again |
| `flash({ device })` | job | Flashes the firmware; `device` is a board id or a serial port such as `COM7` |
| `wifi({ device, ssid, password })` | job | Sets up Wi-Fi over the serial port; an empty `ssid` forgets it |
| `rename({ device, name })` | job | Renames the board (1 to 32 bytes) |
| `diagnose({ device })` | job | Asks a board or a serial port and says what is wrong |
| `scan({})` | job | Looks for boards nearby over Bluetooth for six seconds and answers like `bluetooth()` |
| `pair({ device })` | job | Pairs over Bluetooth: `device` is a board on USB (it gets its key first), the id of a board whose key this computer has, or the Bluetooth address of a board nearby |
| `unpair({ device })` | job | Removes the Bluetooth pairing from Windows and the extension and keeps the key for Wi-Fi; `device` is the board's id or Bluetooth address |

Events (`events.on(topic, ({ value }) => …)`): `board.added`, `board.changed`, `board.removed`, `ports.changed`, `flash.progress`, `flash.done`, `board.diagnosed`, `bluetooth.changed`.

## Firmware and protocol

- Firmware: with ESP-IDF v6.1 installed, run `tools\build_firmware.ps1`; it builds the six chips, merges their images and updates `firmware/release/firmware.json`. See [firmware/README.md](firmware/README.md).
- Protocol: [protocol/SPEC.md](protocol/SPEC.md). Change it in this order: SPEC → `protocol/gen_vectors.py` → `gs_esp32_protocol.h` → tests; never edit the vectors by hand.

## Not done yet

- The images in `firmware/release/` are still 1.0.2: rebuild them from the 1.1.0 sources with `tools\build_firmware.ps1` (ESP-IDF v6.1) so that the firmware the extension brings has the Bluetooth service for the computer.

The download address is added by the maintainers when they publish a release.
