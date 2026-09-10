# Ludant protocol v2 fixtures

These language-neutral JSON documents are the contract shared by the Arduino
production runtime, the ESP-IDF reference runtime, and the iOS client. They are
kept beside the firmware so a protocol change is reviewable without opening an
iOS project file.

The status characteristic transports each JSON document in compact `F2` frames
(`messageId`, `chunkIndex`, `chunkCount`, `payloadLength`, CRC16, payload). A
configuration response is authoritative only when its returned revision, hash,
and module list match the requested draft.
