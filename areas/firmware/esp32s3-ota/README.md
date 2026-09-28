# Ludant ESP32-S3 firmware

The ESP-IDF firmware is the canonical Ludant runtime; an Arduino sketch is also
provided for development. Product firmware is intentionally left open for owner
reflashing over the ESP32-S3 ROM USB downloader.

For the intended architecture and guidance for LLM-assisted changes, see
[AGENTS.md](AGENTS.md). In short, keep the ESP32 runtime deterministic and
minimal; the Swift iOS app owns domain policy, orchestration, and UX.

## Hardware and flash assumptions

- Target: ESP32-S3 development board.
- The standard BOOT button is assumed to be GPIO0 and active low. Change `CONFIG_LUDANT_BOOT_BUTTON_GPIO` in `idf.py menuconfig` for another board.
- The custom partition table assumes a 4 MiB flash device and provides two 1.25 MiB OTA slots. Larger flash can use the remaining space by enlarging the app slots in `partitions.csv`.
- The initial image is flashed over USB. Later Ludant images use the inactive OTA slot over BLE.
- Product builds leave Secure Boot, flash encryption, and eFuse anti-rollback disabled. They do not intentionally burn eFuses or disable ROM USB download mode.

## Build, flash, and monitor

For local development, after sourcing the ESP-IDF export script:

```sh
idf.py set-target esp32s3
idf.py build
```

Use `npx nx run esp32s3-ota-firmware:flash-monitor` for USB flashing. The
workspace flash target explicitly disables and verifies irreversible security
options before it writes to the board.

The same operations are available from the Nx workspace root:

```sh
npx nx run esp32s3-ota-firmware:set-target
npx nx run esp32s3-ota-firmware:build
npx nx run esp32s3-ota-firmware:flash-monitor
```

For the validated full USB install of the latest signed Ludant release, use
`npm run firmware:install` from the workspace root. It checks the build
configuration and refuses to overwrite a board that already has Secure Boot or
flash encryption enabled. Owners can erase and reflash an unlocked board with
their own firmware using the standard ESP32-S3 USB ROM download tools.

Do not enable Secure Boot, flash encryption in development or release mode,
secure UART/USB download mode, or app anti-rollback in a product build. Those
features can change one-way eFuse state on first boot. Existing devices whose
fuses are already burned cannot be converted back to an unlocked state.

If the port is not detected automatically, pass `--port /dev/cu.usbmodemXXXX`
to `npm run firmware:install`.

The build generates `build/ludant_esp32s3_ota.bin`. That is the application image the iOS client must send over BLE for later application updates. Do not send the bootloader, partition-table, or OTA data binaries.

## OTA safety behaviour

Production OTA `BEGIN` is accepted only while the configured BOOT button is
held. Development ESP-IDF builds can opt into the timed boot window through
menuconfig; Arduino builds use `-DLUDANT_OTA_DEVELOPMENT_WINDOW_SECONDS=120`.
Every accepted artifact must also pass Ed25519 signature verification using the
release public key compiled into the firmware.

The transfer is accepted in this order:

1. Subscribe to Status notifications.
2. Write a JSON `begin` command to Control.
3. Wait for `PREPARING`, then `READY`.
4. Write the raw `.bin` bytes sequentially to Firmware Data. Fast-capable
   clients use write-without-response packets in an eight-packet window;
   compatible clients may use write-with-response packets. Do not add JSON or
   Base64 around the bytes.
5. Wait for the final `PROGRESS:<size>:<size>` confirmation, then write
   `{"command":"end"}` to Control.
6. Wait for `VERIFYING`, then `SUCCESS`. The device waits about one second and reboots.

`esp_ota_end()` validates the ESP application image. Firmware data is copied
from the BLE callback into a bounded 512-byte packet queue; flash writes and
SHA-256 updates run serially in the main loop/OTA worker. `END` waits for that
queue to drain, and the final `PROGRESS:<size>:<size>` is sent only after the
last packet is committed. The firmware then compares a streaming SHA-256
digest against the lowercase hex value supplied in `BEGIN`, and calls
`esp_ota_set_boot_partition()` only after both validations succeed. Disconnects,
aborts, short transfers, oversize images, queue overflow, write failures,
invalid images, and hash mismatches leave the currently running app selected.

Two-slot OTA rollback is enabled in `sdkconfig.defaults`. The startup path confirms a pending image with `esp_ota_mark_app_valid_cancel_rollback()` after basic initialization. This rollback is separate from the eFuse anti-rollback feature, which remains disabled. Future application firmware that uses this OTA layer should call the same API as early as possible after its own startup checks; an unconfirmed new OTA image can be rolled back by the ESP-IDF bootloader after a failed first boot.

SHA-256 provides transfer integrity while the Ed25519 signature provides
publisher authenticity. The release public key is supplied as
`CONFIG_LUDANT_OTA_PUBLIC_KEY_DER_HEX` for ESP-IDF and
`LUDANT_OTA_PUBLIC_KEY_DER_HEX` for Arduino. An empty key rejects OTA.

Create a package for iOS from an application image with:

```sh
LUDANT_OTA_SIGNING_PRIVATE_KEY="$(cat release-ed25519-private.pem)" \
node tools/create-firmware-artifact.mjs build/ludant_esp32s3_ota.bin \
  release/ludant-2.0.0.ludantfirmware --version arduino-modules-2.0.0
```

The command prints both public-key encodings: inject `publicKeyDerHex` into
ESP-IDF/Arduino firmware as `LUDANT_OTA_PUBLIC_KEY_DER_HEX`, and inject
`publicKeyRawBase64` into the iOS `LUDANT_OTA_PUBLIC_KEY` build setting. Never
commit the private key or send bootloader, partition-table, or OTA-data
binaries.

### Release open ESP-IDF firmware

Product ESP-IDF releases are built with Secure Boot, flash encryption, and
eFuse anti-rollback disabled. Release signing here means the Ludant BLE package
signature only; no hardware Secure Boot key is required. From the workspace
root, use:

```sh
LUDANT_OTA_SIGNING_KEY_FILE="$HOME/.config/ludant/keys/ota-ed25519-private.pem" \
npm run firmware:bump:patch
```

Use the minor or major bump command when appropriate. To build a specific
version, run `npm run firmware:release -- 2.1.0`. Both paths produce an
unlocked USB flash image while retaining signed BLE OTA packages.

### Increment and release an Arduino version

The workspace includes a guarded release command that increments the current
Arduino sketch version, injects the signing public key, compiles the application
image, creates the signed `.ludantfirmware` package in the iOS resource folder,
and updates `FirmwareCatalog.json`:

```sh
npm run firmware:release:arduino -- patch
```

Use `minor` or `major` instead of `patch` when appropriate. Add `--dry-run` to
preview the next version and asset path without changing files. Set
`LUDANT_ARDUINO_FQBN` when using a different ESP32-S3 board definition. The
command refuses to overwrite an existing catalog version or asset and restores
source edits if compilation or signing fails. It requires `arduino-cli` and an
Ed25519 release key; the private key is never written to the repository.

## Manual test

1. Install the open firmware over USB with `npm run firmware:install` (hold BOOT while resetting if the board needs ROM download mode).
2. Open the serial monitor and confirm `Ludant` is advertising.
3. Use a BLE inspector to discover service `7A910000-4C5E-4A9B-8F23-91F4A7D10000`.
4. Enable notifications on Status and read Device Info.
5. Compute the SHA-256 of the exact `build/ludant_esp32s3_ota.bin` file sent.
6. Send `BEGIN` with the exact byte size, lowercase SHA-256, and a version string.
7. Send all file bytes in order to Firmware Data, using the negotiated BLE packet size.
8. Send `END`, observe verification and reboot, then reconnect and read Device Info.
9. Test ABORT, disconnect during transfer, an altered SHA-256, and a truncated transfer. In each case the current app should remain bootable and the device should advertise again.

See [BLE_OTA_PROTOCOL.md](BLE_OTA_PROTOCOL.md) for the characteristic contract and Swift client sequence.

## Arduino IDE module firmware

The complete Arduino IDE module sketch is available at
`arduino/ludant_esp32s3_ota/ludant_esp32s3_ota.ino`. Install the Espressif ESP32 Arduino board
package, select an ESP32-S3 board, select the board's USB port, and upload
the sketch. Open Serial Monitor at 115200 baud to confirm that it is
advertising as `Ludant`.

The sketch is the primary installation path. It uses the same BLE UUIDs and
OTA commands as the ESP-IDF firmware and includes the built-in MPU6050, BME280,
soil-moisture, relay, persisted module configuration, atomic `apply_modules`,
and live telemetry support. Its status responses are queued and fragmented so
they remain safe on iOS links that negotiate the default 23-byte BLE MTU.
Upload it from Arduino IDE, reconnect the controller in the iOS app, and publish
the staged module configuration.

For the Motion sensor, wire VCC to 3.3 V, GND to GND, SDA to the configured SDA
GPIO (the new-module default is GPIO8), and SCL to the configured SCL GPIO (the
default is GPIO9). The default MPU6050 address is `0x68`; use `0x69` when AD0 is
high. The sketch wakes the MPU6050 before reading it. If the sensor does not
respond, the iOS live-data screen reports the address and GPIOs instead of
silently showing an empty stream.

The ESP-IDF project remains available as an alternative firmware build, but the
iOS module workflow no longer requires or bundles an ESP-IDF `.bin` file.

### Arduino stack regression check

Version `arduino-modules-1.1.5` adds protocol-v2 command framing, calibrated BME280 telemetry, clean telemetry
session recovery on reconnect, revisioned two-slot
configuration persistence, MPU6050 identity checks, safe I²C bus switching,
cached BME280 calibration, and reports I2C sensor
failures. It retains the `arduino-modules-1.0.2` stack-safety changes,
which moved module candidate arrays and the command receive buffer out of the
8 KB loop task stack. They belong exclusively to
setup/loop; BLE callbacks must continue to queue module commands. The live module
list remains separate from the candidate list until validation and persistence
succeed. Serial output reports `command complete, loop stack minimum free=...`
after each command, for checking stack headroom on the physical device.

Compile with GCC stack reports, then check the sensitive function budgets:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
  --build-path /private/tmp/ludant-stack-audit-1.0.2 \
  --build-property compiler.cpp.extra_flags=-fstack-usage \
  areas/firmware/esp32s3-ota/arduino/ludant_esp32s3_ota
node areas/firmware/esp32s3-ota/tools/check-arduino-stack.mjs \
  /private/tmp/ludant-stack-audit-1.0.2/sketch/ludant_esp32s3_ota.ino.cpp.su
```

These are per-function static budgets, not a measurement of the complete runtime
call stack. On hardware, verify adding a sensor, publishing again, rebooting
with the saved sensor, and removing it without a panic. The iOS device-info log
must show `arduino-modules-1.1.5` and protocol `2` when testing this revision.
