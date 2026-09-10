# Ludant ESP32-S3 firmware

The Arduino IDE sketch is the primary installation path for Ludant electronics
controllers. This directory also contains the equivalent ESP-IDF implementation
for builds that need it; both runtimes use the same BLE protocol.

## Hardware and flash assumptions

- Target: ESP32-S3 development board.
- The standard BOOT button is assumed to be GPIO0 and active low. Change `CONFIG_LUDANT_BOOT_BUTTON_GPIO` in `idf.py menuconfig` for another board.
- The custom partition table assumes a 4 MiB flash device. It provides a 1 MiB factory image and two 1.25 MiB OTA slots. Larger flash can use the remaining space by enlarging the app slots in `partitions.csv`.
- The initial factory image is flashed over USB. Later images are written only to the inactive OTA slot.

## Build, flash, and monitor

From this directory, after sourcing the ESP-IDF export script:

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

The same operations are available from the Nx workspace root:

```sh
npx nx run esp32s3-ota-firmware:set-target
npx nx run esp32s3-ota-firmware:build
npx nx run esp32s3-ota-firmware:flash-monitor
```

If the port is not detected automatically, use `idf.py -p /dev/cu.usbmodemXXXX flash monitor`.

The build generates `build/ludant_esp32s3_ota.bin`. That is the application image the iOS client must send over BLE for later application updates. Do not send the bootloader, partition-table, or OTA data binaries.

## OTA safety behaviour

OTA `BEGIN` is accepted for 120 seconds after boot, or whenever the configured BOOT button is held. An update writes to `esp_ota_get_next_update_partition()` and never targets the running partition.

The transfer is accepted in this order:

1. Subscribe to Status notifications.
2. Write a JSON `begin` command to Control.
3. Wait for `READY`.
4. Write the raw `.bin` bytes sequentially to Firmware Data. Use write-without-response where supported and do not add JSON or Base64 around the bytes.
5. Write `{"command":"end"}` to Control.
6. Wait for `VERIFYING`, then `SUCCESS`. The device waits about one second and reboots.

`esp_ota_end()` validates the ESP application image. The firmware also compares a streaming SHA-256 digest against the lowercase hex value supplied in `BEGIN`, then calls `esp_ota_set_boot_partition()` only after both validations succeed. Disconnects, aborts, short transfers, oversize images, write failures, invalid images, and hash mismatches leave the currently running app selected.

Rollback is enabled in `sdkconfig.defaults`. The startup path confirms a pending image with `esp_ota_mark_app_valid_cancel_rollback()` after basic initialization. Future application firmware that uses this OTA layer should call the same API as early as possible after its own startup checks; an unconfirmed new OTA image can be rolled back by the ESP-IDF bootloader after a failed first boot.

SHA-256 proves transfer integrity, not publisher authenticity. Before production use, add authentication and signed-image verification at the `OtaPermission`/`OtaManager` boundary and enable ESP-IDF secure boot and flash encryption as appropriate.

## Manual test

1. Hold BOOT while flashing the initial factory image over USB.
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
