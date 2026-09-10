# Ludant BLE OTA protocol

## UUIDs

| Item | UUID | Properties |
| --- | --- | --- |
| OTA service | `7A910000-4C5E-4A9B-8F23-91F4A7D10000` | Primary service |
| Control | `7A910001-4C5E-4A9B-8F23-91F4A7D10000` | Write |
| Firmware Data | `7A910002-4C5E-4A9B-8F23-91F4A7D10000` | Write, Write Without Response |
| Status | `7A910003-4C5E-4A9B-8F23-91F4A7D10000` | Read, Notify |
| Device Info | `7A910004-4C5E-4A9B-8F23-91F4A7D10000` | Read |

The device advertises as `Ludant` and exposes the OTA service UUID for discovery.

## Permission window

`begin` is accepted only during the first 120 seconds after boot or while the configured BOOT button is held. This is an MVP authorization gate. It is intentionally structured so a future challenge/response or signed-update policy can be added without changing the data transport.

## Control messages

Control messages are UTF-8 JSON.

### Begin

```json
{
  "command": "begin",
  "size": 1048576,
  "sha256": "64 lowercase hex characters",
  "version": "0.1.1"
}
```

The ESP32 selects the inactive OTA app partition, erases it through `esp_ota_begin()`, resets its byte counter, and begins a streaming SHA-256 calculation. It notifies `READY` only after that setup succeeds.

### End

```json
{"command":"end"}
```

The device requires the received byte count to equal `size`, calls `esp_ota_end()` for image validation, compares SHA-256, sets the new partition bootable, notifies `SUCCESS`, waits about one second, and restarts.

### Abort

```json
{"command":"abort"}
```

The active OTA handle is aborted and the current boot partition is not changed.

## Firmware data

Firmware Data contains the exact raw bytes of the ESP-IDF application `.bin`. Do not send JSON, Base64, a file header, or a checksum prefix. The client may use the negotiated ATT MTU and write-without-response; the ESP32 does not assume a 20-byte payload. Every write is appended in order, and a write that would exceed the declared size fails and aborts the update.

## Status messages

Status notifications are UTF-8 strings. New controller/runtime messages use
compact protocol-v2 frames:

```text
F2<messageId:2 base36><chunkIndex:2 base36><chunkCount:3 base36><payloadLength:3 base36><crc16:4 hex><payload:0-4 bytes>
```

The CRC-16 is calculated over the complete UTF-8 JSON payload. The iOS client
rejects incomplete, incorrectly sized, or invalid-CRC messages. Older
unframed status strings and `C<id>:<index>/<total>:` fragments remain accepted
for recovery, but are not emitted by the current Arduino firmware.

Legacy OTA status notifications remain plain UTF-8 strings:

```text
READY
PROGRESS:32768:1048576
VERIFYING
SUCCESS
ABORTED
ERROR:<code>:<description>
```

Progress is throttled to approximately every 32 KiB and at completion. A client should treat any `ERROR:` or `ABORTED` message as a failed transfer and restart from `begin` after reconnecting or re-entering the permission window.

## Device Info

Example read response:

```json
{
  "device": "electronics-controller",
  "chip": "ESP32-S3",
  "firmware": "arduino-modules-1.1.5",
  "otaProtocol": 1,
  "protocolVersion": 2,
  "supportsModules": true,
  "capabilities": [{"driverId":"mpu6050","version":"1.0.0"}]
}
```

## Swift/CoreBluetooth sequence

The iOS client should:

1. Scan for the OTA service UUID and connect.
2. Discover the five characteristics.
3. Subscribe to Status notifications before sending Control.
4. Read Device Info if desired.
5. Ensure the device is within its OTA permission window, then write `begin` to Control.
6. Wait for `READY` (and handle `ERROR:`).
7. Split the exact application `.bin` bytes into packets no larger than the negotiated `maximumWriteValueLength(for: .withoutResponse)` and call `writeValue(_:for:type: .withoutResponse)` in order. If flow control is needed, wait for `canSendWriteWithoutResponse` before continuing. The iOS client owns packet pacing; the ESP32 only guarantees sequential handling of each ATT write.
8. After every byte is sent, write `end` to Control.
9. Wait for `VERIFYING` and `SUCCESS`. The connection will close when the ESP32 reboots.
10. Reconnect after advertising resumes and read Device Info to confirm the new version.

The SHA-256 must be calculated over the exact bytes transmitted, not over the source project or an archive. Use lowercase hexadecimal in the JSON request.

## Controller messages

The same Control and Status characteristics also carry the controller protocol. OTA
commands continue to use the text status messages above; all controller replies and
telemetry packets are UTF-8 JSON. Requests are serialized by the client and include a
unique `requestId`.

```json
{"command":"get_state","requestId":"..."}
{"command":"apply_modules","requestId":"...","baseRevision":11,"modules":[]}
{"command":"configure_module","requestId":"...","module":{}}
{"command":"remove_module","requestId":"...","instanceId":"..."}
{"command":"set_module_enabled","requestId":"...","instanceId":"...","enabled":true}
{"command":"start_telemetry","requestId":"...","instanceId":"...","intervalMs":1000}
{"command":"stop_telemetry","requestId":"...","instanceId":"..."}
{"command":"set_live_interval","requestId":"...","instanceId":"...","intervalMs":250}
```

`apply_modules` replaces the complete desired module list in one operation. The
request includes the revision from which the local draft was created. The
controller validates every module, GPIO conflict, I2C address conflict, and driver,
rejects stale revisions, then writes a new revisioned configuration slot before
activating it. A failed request leaves the previously persisted module configuration
unchanged. A successful request returns the resulting complete controller state in
`payload.state`, including `configRevision` and `configHash`.

Control requests that fit within the negotiated write size are sent as plain JSON.
Larger requests use binary `C2` command frames on the same Control characteristic:

```text
C2 version(1) messageId(u16 LE) chunkIndex(u16 LE) chunkCount(u16 LE)
   messageLength(u16 LE) crc16(u16 LE) payload(bytes)
```

The frame header is 12 bytes. The client sends frames sequentially and waits for
the BLE write acknowledgement before sending the next one. The ESP32 requires
frames in order, reassembles the complete JSON payload, validates its length and
CRC-16, and only then queues it for command processing. Partial or expired messages
are discarded without changing controller state. The maximum complete command is
2047 bytes, matching the controller command buffer.

Replies have the following envelope:

```json
{"type":"response","requestId":"...","protocolVersion":2,"ok":true,"configRevision":12,"configHash":"...","payload":{}}
```

Telemetry is emitted as:

```json
{"type":"telemetry","protocolVersion":2,"sequence":42,"uptimeMs":123456,"deviceId":"...","instanceId":"...","quality":"ok","error":null,"values":{}}
```

Module configurations are persisted on the controller only after validation. The
initial firmware advertises MPU6050, BME280, analog soil-moisture, and relay drivers;
future drivers can be added to the module registry without changing the BLE transport.
