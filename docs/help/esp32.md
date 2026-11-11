The ESP32 touch box is an ESP32 development board with the touch box firmware on it: the phone takes it for a plugged-in touch screen, keyboard, mouse and gamepad, so it works even with USB debugging off. This extension finds the boards, shows them on the Devices page, flashes their firmware and sends them the touches over a serial port (or over Wi-Fi).

## Pick a board

- **ESP32-S3, ESP32-S2, ESP32-P4**: plug it into the phone with a USB cable and the phone takes it for a hardware touch screen. The P4 uses high-speed USB for the least delay; it has no Wi-Fi, so it talks to this computer over the serial port only.
- **Classic ESP32, ESP32-C3, ESP32-C6**: these cannot be a USB device, so they reach the phone over Bluetooth. Pair "GameScrcpy XXXX" once in the phone's Bluetooth settings; afterwards it reconnects by itself, a few milliseconds slower than USB.
- The board needs at least 4 MB of flash; common development boards have that.

## Wiring {Two cables}

1. **To the phone**: on the S3 and S2 use the native port marked "USB", on the P4 the "USB 2.0 OTG HS" port, with an OTG cable to the phone. Bluetooth boards pair with the phone instead.
2. **To this computer**: use the board's other port (the USB-to-serial one, often marked "UART" or "COM"). The C3, C6 and P4 can also use the chip's own USB serial port. Windows has the drivers for these ports already.
3. **Find it in GameScrcpy**: with the extension on, it tries the Espressif, CH34x and CP210x serial ports one after another and the board shows up on the Devices page as a touch box. Click Control on its card to work the phone without a picture, or choose the touch method ESP32 in a phone's settings so that a mirrored phone is touched through the board.

> [!TIP]
> A small S2 board with only the native USB port needs a USB-to-serial adapter on GPIO43 (TX) and GPIO44 (RX) for this computer.

## Where the settings are

- **The board's card on the Devices page**: Settings opens the board's own window: what it is, its firmware, Try it, Wi-Fi, its name and a diagnosis. More holds Try it, Flash firmware, Diagnose and Disconnect.
- **The extension's card on the Extensions page**: Settings shows every board found and lets you flash the firmware to any serial port, also to a board that does not answer yet.
- **A phone's settings**: the touch method ESP32, and this extension's settings for that phone: the serial port (auto or a port such as COM7), the connection (serial port or Wi-Fi) and the Wi-Fi address. The values for all phones are in the extension's settings.

## Flash or upgrade the firmware

On the board's card choose Flash firmware, or in the extension's settings pick the serial port the board is plugged into and click Flash firmware. The extension brings the firmware and the flashing tool along and usually puts the board into download mode by itself; writing takes about half a minute, and after the check the board restarts. When a new version of the extension brings newer firmware, the board's window shows that an upgrade is available.

- **When to hold BOOT**: if it says the board did not enter download mode, hold the board's BOOT button (IO0 on some boards) while plugging in the cable (or press RST / EN once), let go of BOOT, then flash again.
- **Old firmware**: a board that still runs the old QineTouch firmware shows "Old firmware" on its card. The old firmware only touches; keys, the mouse, the gamepad and Wi-Fi need the new one.

## Connect over Wi-Fi

While the board is on the serial port, open its window from its card, enter the network name and password in the Wi-Fi group and click Save Wi-Fi; the extension also makes a key that only this computer knows. Then choose Wi-Fi as the connection in the extension's settings (or in one phone's settings): with the address left empty it finds the board by itself, or enter its IP address. Wi-Fi adds 1 to 3 ms of delay and sometimes jitters.

## What the messages mean

- **"Nothing answers on this port"**: the board sent nothing at all. On an ESP32-S3 or S2 with two USB ports, the computer must use the port marked "COM" or "UART" (the USB-to-serial chip); the port marked "USB" goes to the phone. Check the cable too.
- **"The board keeps restarting (reset reason …)"**: it prints its boot messages again and again instead of answering. Flash the firmware again; if it keeps happening, check that the board has 4 MB of flash and enough power.
- **"The board is in download mode"**: it waits for a firmware writer. Press RST (EN) on the board once, then look for boards again.
- **"The bytes on this port do not look like the ESP32 touch box"**: something answers, but it is not this protocol: the wrong port, a serial speed the cable cannot keep, or a board that restarts before it can talk.
- **"The board runs the old QineTouch firmware"**: only touches can be sent before the new firmware is flashed.
- **"In use by …"**: another extension or your own logic has the serial port; the board comes back when it lets go, or when you look for boards again.
- Diagnose on the board's card asks the board what it is and says what is wrong; the board's window shows the answer.

## Safety and tips

- When nothing comes from this computer for 2 seconds, the board lifts every finger, lets go of every key and mouse button and centres the gamepad, so no finger stays stuck on the phone.
- Try it taps the middle of the phone's screen once, to check that the phone side is connected.
- While a phone is controlled through the board, the board's card says it is in use and flashing waits until that window is closed.
- With the ESP32's keyboard attached, Android stops showing the on-screen keyboard by default; turn on "Show virtual keyboard" in the phone's physical keyboard settings when you need it.
