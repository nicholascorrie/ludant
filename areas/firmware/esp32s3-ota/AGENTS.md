# Firmware/app architecture guidance

This directory contains the code that runs on the ESP32-S3 controller. The
deliberate architectural boundary is:

> Keep the ESP32 firmware as dumb, deterministic, and stable as possible.
> The Swift iOS app owns the domain model, product policy, orchestration, and
> user experience.

This is guidance for LLMs and contributors modifying this area. Read it before
changing firmware, BLE protocol, OTA, or the matching Swift client.

## What belongs on the ESP32

The firmware should provide a small, boring device runtime:

- Read physical inputs and drive hardware.
- Maintain the minimum device/module state needed to operate safely.
- Expose stable BLE characteristics and parse well-defined commands.
- Queue BLE work out of callbacks and process it serially in the main loop or
  an appropriate FreeRTOS task.
- Transfer OTA bytes, verify size/hash/signature, enforce the physical BOOT
  permission, select the inactive partition, reboot, and support rollback.
- Return short, stable status codes and enough diagnostic information to debug
  transport or hardware failures.

Firmware must fail closed for malformed commands, invalid metadata, bad
signatures, hash mismatches, oversize images, invalid partitions, disconnects,
and unauthorized OTA attempts. Never add a development bypass to production
behaviour just to make the app flow easier.

## What belongs in the Swift app

The iOS app is the domain and orchestration layer. It should own:

- Firmware catalogs, release selection, version policy, compatibility rules,
  and release notes.
- Artifact loading, local validation, signing-key configuration, and the OTA
  workflow state machine shown to the user.
- Retries, fallbacks for older firmware, telemetry/configuration workflows,
  and user-facing error messages.
- Decisions such as which release to offer, whether an update is useful, and
  how the user should be guided through holding BOOT.

The app may preflight or poll the controller for a status hint such as
`AUTHORIZATION_READY`, but the firmware's `begin` handler remains the final
security boundary. An app-side check must never replace device-side
permission, signature, hash, or image validation.

## Rules for protocol changes

1. Prefer extending the existing small command/status contract over moving
   domain logic into firmware.
2. Keep commands explicit, bounded, and backward-compatible where practical.
   Do not make the device infer app concepts from catalog or UI data.
3. Update `BLE_OTA_PROTOCOL.md` and the matching Swift transport/client code
   whenever a command, response, framing rule, or OTA state changes.
4. Keep BLE callbacks thin: validate sizes, copy/queue data, and return. Do
   not perform long operations, large stack allocations, or complex module
   workflows in callbacks.
5. Do not log private signing material. Public-key material may be diagnosed,
   but signatures and firmware bytes should not be dumped in normal logs.
6. If a change requires new policy, product logic, presentation text, or
   release behaviour, implement that in the Swift app unless it is required
   for device safety or security.

## OTA invariants

- The iOS package signature and the public key compiled into the controller
  must come from the same release key.
- A generated `.ludantfirmware` package contains the application image only;
  never send the bootloader, partition table, or OTA-data image over BLE.
- The device must verify the exact received bytes before selecting the new
  partition.
- `ota_authorization_status` is a status signal for the app; `begin` still
  rechecks BOOT and all artifact metadata.
- Keep Arduino and ESP-IDF implementations behaviourally equivalent when the
  shared BLE/OTA protocol is changed.

## Change checklist

Before finishing a change, check the smallest relevant set of:

- Arduino compile and, when applicable, ESP-IDF build.
- iOS Xcode build and Swift compile.
- OTA protocol documentation and release tooling.
- Signature/key consistency and exact artifact size/hash.
- Disconnect, abort, malformed-input, and rollback behaviour.

When uncertain where new logic belongs, put policy and interpretation in
Swift; put only the minimum enforcement and hardware/transport mechanics on
the ESP32.
