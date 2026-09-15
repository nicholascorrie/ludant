#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ludant {

static constexpr size_t kBinaryTelemetryFrameLength = 20;
static constexpr uint8_t kBinaryTelemetryFrameMagic = 0x54;
static constexpr uint8_t kBinaryTelemetryVersion = 2;
static constexpr uint8_t kBinaryTelemetryFrameStart = 0x01;
static constexpr uint8_t kBinaryTelemetryFrameEnd = 0x02;
static constexpr size_t kBinaryTelemetryMaxFields = 16;
static constexpr size_t kBinaryTelemetryFieldNameLength = 31;
static constexpr size_t kBinaryTelemetryErrorLength = 95;
static constexpr size_t kBinaryTelemetryMaxPayloadLength =
    3 + kBinaryTelemetryErrorLength +
    kBinaryTelemetryMaxFields * (1 + kBinaryTelemetryFieldNameLength + sizeof(float));

// This is the in-memory representation queued by firmware. The wire format
// is encoded into <=20-byte notifications by the BLE layer, so future module
// fields do not require another characteristic or another fixed struct.
struct BinaryTelemetryField {
    char name[kBinaryTelemetryFieldNameLength + 1];
    float value;
};

struct BinaryTelemetryPacket {
    uint16_t sequence;
    uint32_t uptime_ms;
    uint8_t quality;
    uint8_t field_count;
    char error[kBinaryTelemetryErrorLength + 1];
    BinaryTelemetryField fields[kBinaryTelemetryMaxFields];
};

inline size_t encodeBinaryTelemetryPayload(const BinaryTelemetryPacket& packet,
                                           uint8_t* output, size_t capacity) {
    if (output == nullptr || capacity < 3) return 0;
    size_t offset = 0;
    output[offset++] = packet.quality;
    const uint8_t field_count = packet.field_count > kBinaryTelemetryMaxFields
        ? static_cast<uint8_t>(kBinaryTelemetryMaxFields)
        : packet.field_count;
    output[offset++] = field_count;
    const size_t error_length = std::strlen(packet.error) > kBinaryTelemetryErrorLength
        ? kBinaryTelemetryErrorLength
        : std::strlen(packet.error);
    output[offset++] = static_cast<uint8_t>(error_length);
    if (offset + error_length > capacity) return 0;
    std::memcpy(output + offset, packet.error, error_length);
    offset += error_length;
    for (uint8_t index = 0; index < field_count; ++index) {
        const auto& field = packet.fields[index];
        const size_t name_length = std::strlen(field.name) > kBinaryTelemetryFieldNameLength
            ? kBinaryTelemetryFieldNameLength
            : std::strlen(field.name);
        if (offset + 1 + name_length + sizeof(float) > capacity) return 0;
        output[offset++] = static_cast<uint8_t>(name_length);
        std::memcpy(output + offset, field.name, name_length);
        offset += name_length;
        std::memcpy(output + offset, &field.value, sizeof(field.value));
        offset += sizeof(field.value);
    }
    return offset;
}

struct __attribute__((packed)) BinaryTelemetrySample {
    uint16_t sequence;
    uint32_t uptime_ms;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    int16_t temperature;
};

static_assert(sizeof(BinaryTelemetrySample) == kBinaryTelemetryFrameLength,
              "Binary telemetry frame must fit one BLE notification");

inline int16_t quantizeTelemetry(double value, double scale) {
    const double scaled = value * scale;
    if (scaled >= 32767.0) return 32767;
    if (scaled <= -32768.0) return -32768;
    return static_cast<int16_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

} // namespace ludant
