# Ludant BLE OTA protocol v2

## UUIDs

| Item | UUID | Properties |
| --- | --- | --- |
| OTA service | `7A910000-4C5E-4A9B-8F23-91F4A7D10000` | Primary service |
| Control | `7A910001-4C5E-4A9B-8F23-91F4A7D10000` | Write |
| Firmware Data | `7A910002-4C5E-4A9B-8F23-91F4A7D10000` | Write, Write Without Response |
| Status | `7A910003-4C5E-4A9B-8F23-91F4A7D10000` | Read, Notify |
| Device Info | `7A910004-4C5E-4A9B-8F23-91F4A7D10000` | Read |
| Telemetry | `7A910005-4C5E-4A9B-8F23-91F4A7D10000` | Notify |

The device advertises as `Ludant` and exposes the OTA service UUID.

## Authorization and authenticity

Production `begin` requests are accepted only while the configured BOOT button
is held. Development builds may explicitly enable the timed boot window. The
policy is reported as `otaAuthorization` in Device Info, and authorization
failures use `ERROR:WINDOW_CLOSED:<description>`.

The detached Ed25519 signature signs the raw 32-byte SHA-256 digest of
`firmware.bin`. The public key is compiled into firmware and iOS. An empty or
invalid key fails closed.

## Control messages

Control messages are UTF-8 JSON. The required begin shape is:

Before `begin`, clients may send:

```json
{"command":"ota_authorization_status"}
```

The controller reports `WAITING_FOR_BOOT` until the physical BOOT button is
held, then reports `AUTHORIZATION_READY`. This is a preflight hint only;
`begin` re-checks authorization immediately and remains the security boundary.

```json
{
  "command":"begin",
  "product":"ludant-esp32s3",
  "hardware":"ESP32-S3",
  "size":1048576,
  "sha256":"lowercase-64-character-sha256",
  "version":"arduino-modules-2.0.0",
  "otaProtocol":2,
  "signature":{"algorithm":"ed25519","value":"base64","keyId":"release-1"}
}
```

The ESP32 validates authorization, product, hardware, OTA protocol, version,
anti-rollback floor, signature, image size, and inactive partition before
preparing the target. It reports `READY` only after preparation succeeds.
Preparation may emit `PREPARING` first.

End:

```json
{"command":"end"}
```

The device requires the received byte count to equal `size`, validates the ESP
application image, compares SHA-256, checks the embedded image version against
`version`, and selects the new boot partition only after every check passes.
It then reports `SUCCESS`, waits briefly, and restarts.

Abort:

```json
{"command":"abort"}
```

The active OTA handle is aborted and the current boot partition is unchanged.

## Firmware data

Firmware Data contains the exact raw bytes of the application `.bin`. Do not
send JSON, Base64, a file header, or a checksum prefix. The client owns packet
sizing and pacing. Fast-capable iOS clients send sequential Write Without
Response packets in an eight-packet window, waiting for CoreBluetooth capacity
and device `PROGRESS` credits. Compatible clients may use sequential Write With
Response packets as a fallback. Firmware accepts non-empty sequential ATT
writes up to 512 bytes, never assumes 20-byte packets, and copies each packet
into a bounded queue before returning from the BLE callback. Flash writes and
SHA-256 updates run serially outside the callback. `END` drains this queue
before checking the received size or beginning verification.

The final `PROGRESS:<size>:<size>` notification is emitted only after the last
queued packet has been written to flash. The client must receive that status
before sending `END`.

## Status messages and errors

OTA statuses remain plain UTF-8 strings:

```text
PREPARING
READY
WAITING_FOR_BOOT
AUTHORIZATION_READY
PROGRESS:<receivedBytes>:<expectedBytes>
VERIFYING
SUCCESS
ABORTED
ERROR:<code>:<description>
```

Progress is emitted approximately every 4 KiB, when the OTA queue drains, and
at completion. Queue-drain progress is required for smaller negotiated BLE
packets so an eight-packet window can always receive a credit. Both
implementations use these error codes where applicable:

`WINDOW_CLOSED`, `ALREADY_ACTIVE`, `INVALID_BEGIN`, `INVALID_VERSION`,
`INCOMPATIBLE_ARTIFACT`, `SIGNATURE_INVALID`, `VERSION_REJECTED`,
`NO_PARTITION`, `IMAGE_TOO_LARGE`, `OTA_BEGIN`, `NOT_ACTIVE`,
`SIZE_MISMATCH`, `IMAGE_INVALID`, `SHA_MISMATCH`, `VERSION_MISMATCH`,
`WRITE_FAILED`, and `SET_BOOT`.

Firmware logs include queue depth, committed byte counts, flash-write duration,
write failures, disconnect aborts, reset reason, and watchdog/startup health
information. These diagnostics stay out of the short status vocabulary so the
OTA state machine remains stable.

## Firmware artifact package

The iOS Files picker accepts a `.ludantfirmware` directory package containing
`manifest.json` and `firmware.bin`. The manifest contains product, hardware,
firmware version, OTA protocol version, binary size, lowercase SHA-256,
capabilities, minimum boot version, optional minimum partition size, release
notes, and Ed25519 signature metadata. Release tooling must generate both files
together and must never include a bootloader, partition table, OTA data binary,
source code, or executable fragment.

## Device Info

Device Info reports at least:

```json
{
  "device":"electronics-controller",
  "chip":"ESP32-S3",
  "hardware":"ESP32-S3",
  "firmware":"arduino-modules-2.0.0",
  "otaProtocol":2,
  "supportsOTA":true,
  "otaCapabilities":["signed","sha256","rollback","sequential_write_with_response","sequential_write_without_response"],
  "protocolVersion":2,
  "otaAuthorization":"physical_button",
  "bootVersion":"1.0.0",
  "otaMaxImageSize":1310720
}
```

## Reboot and rollback

After `SUCCESS`, the client treats disconnect as expected, waits for the same
peripheral to advertise, reconnects, rediscovers characteristics, reads Device
Info, verifies the expected firmware version, and only then reports success.
The new image confirms itself after essential startup, BLE advertising, and
controller health checks. A failed startup remains unconfirmed so the ESP32
bootloader can roll back.

Existing controller protocol-v2 commands remain on Control and Status. Legacy
JSON telemetry continues on Status, while binary telemetry uses the dedicated
Telemetry characteristic. All telemetry is unavailable while OTA is active.

## Binary telemetry

Current and future modules use the same generic binary-v2 stream. New clients
request `"telemetryTransport":"binary-v2"` in the `start_telemetry` or
`set_live_interval` command when the Telemetry characteristic is present.
Older firmware that does not understand this value continues to emit legacy
JSON telemetry over Status; clients omit the field when the characteristic is
not present.

Each logical sample is represented by a binary payload. The payload is split
into BLE notifications when it is larger than one ATT payload, so transport
size is bounded without JSON serialization or status-channel fragmentation.
Every notification is at most 20 bytes on an MTU-23 link.

The first-fragment header is 11 bytes:

| Offset | Size | Field | Encoding |
| ---: | ---: | --- | --- |
| 0 | 1 | magic | `0x54` |
| 1 | 1 | version | `2` |
| 2 | 1 | flags | start=`0x01`, end=`0x02`; both may be set |
| 3 | 2 | sequence | UInt16 little-endian, wraps naturally |
| 5 | 4 | uptimeMs | UInt32 little-endian |
| 9 | 2 | payloadLength | UInt16 little-endian |
| 11 | 0–9 | payload prefix | binary payload bytes |

Continuation headers are 7 bytes and contain magic, version, flags, sequence,
and a UInt16 little-endian payload offset at offset 5. Continuation payload
capacity is 13 bytes. Fragments must arrive in order and retain the same
sequence number.

The payload is encoded as follows:

| Offset | Size | Field | Encoding |
| ---: | ---: | --- | --- |
| 0 | 1 | quality | `0` = ok, non-zero = unavailable |
| 1 | 1 | fieldCount | Maximum 16 |
| 2 | 1 | errorLength | UTF-8 byte count |
| 3 | N | error | UTF-8 error text |
| ... | 1 | nameLength | UTF-8 byte count, maximum 31 |
| ... | N | name | Numeric field name |
| ... | 4 | value | IEEE-754 Float32 little-endian |

The name/value record repeats `fieldCount` times. Existing modules expose
`state`, `moisture`, `accelX/Y/Z`, `gyroX/Y/Z`, `temperature`, `pressure`, and
`humidity` as applicable. iOS converts these fields to the existing normalized
`[String: Double]` telemetry model, so dashboard and history code is
transport-agnostic. The firmware queue is bounded and drops the oldest sample
when full; sequence numbers make that loss observable.

The old fixed 20-byte binary-v1 MPU6050 frame remains decodable by iOS for
compatibility with the interim firmware. It is not used for new module
implementations.

Telemetry uses a bounded firmware queue and prioritizes command responses over
samples. If the queue fills, the oldest sample is discarded; clients detect
that condition from the sequence gap. Sensor errors remain JSON status packets
so unavailable-sensor states retain their diagnostic text.
