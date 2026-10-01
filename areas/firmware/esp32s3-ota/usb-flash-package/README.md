Ludant ESP32-S3 firmware {{VERSION}}

This package installs the first Ludant firmware over USB. It is for ESP32-S3
boards with at least 4 MB of flash. It replaces the bootloader, partition table,
OTA data, and application image. Do not use it on a controller with Secure Boot
or flash encryption enabled.

Before you start

- Use a USB data cable and connect the board directly to your computer.
- Install Python 3.10 or later from [python.org](https://www.python.org/downloads/).
- Install Espressif esptool: `python3 -m pip install esptool` on Mac, or
  `py -3 -m pip install esptool` on Windows.
- If Python or esptool is missing, the installer stops before writing anything.

Flash the board

- Mac: double-click `flash-macos.command`. If macOS blocks it, Control-click
  the file and choose Open.
- Windows: double-click `flash-windows.bat`.
- Choose the ESP32-S3 serial port when prompted. If the board cannot connect,
  hold BOOT, tap RESET, release BOOT, and retry.
- Keep the board powered on after flashing, then return to Ludant and connect
  over Bluetooth.

The installer checks each bundled image, available flash size, and USB download
fuses before it writes. It does not enable eFuse security features.
