# Ludant ESP32-S3 firmware 2.0.45

Use this on a supported ESP32-S3 board with at least 4 MB of flash. Connect it to a Mac or Windows computer with a USB data cable, then open the Ludant firmware link in ESP Launchpad using Chrome or Edge.

Flashing replaces the board’s current firmware and data. It does not burn eFuses, enable Secure Boot, or enable flash encryption. Your board remains open so you can install other compatible firmware later.

ESP Launchpad does not run the USB ZIP installer’s eFuse checks. If the board may already use Secure Boot or flash encryption, use the USB ZIP installer instead; it checks first and refuses before writing.

After flashing, return to Ludant on your iPhone and connect to the controller over Bluetooth.
