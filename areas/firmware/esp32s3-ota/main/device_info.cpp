#include "device_info.hpp"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"

namespace {
constexpr char kNamespace[] = "ludant";
constexpr char kDeviceIdKey[] = "device_id";
constexpr char kNameKey[] = "friendly_name";

std::string escapeJson(const char* value) {
    std::string escaped;
    if (value == nullptr) return escaped;
    for (const unsigned char character : std::string(value)) {
        switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '\"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20) escaped += '?';
                else escaped += static_cast<char>(character);
        }
    }
    return escaped;
}
}

bool DeviceInfo::begin() {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    size_t size = 0;
    if (nvs_get_str(handle, kDeviceIdKey, nullptr, &size) == ESP_OK && size > 1) {
        device_id_.resize(size - 1);
        nvs_get_str(handle, kDeviceIdKey, device_id_.data(), &size);
    } else {
        uint8_t bytes[16]{};
        esp_fill_random(bytes, sizeof(bytes));
        char generated[37]{};
        std::snprintf(generated, sizeof(generated), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                      bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        device_id_ = generated;
        nvs_set_str(handle, kDeviceIdKey, device_id_.c_str());
    }
    size = 0;
    if (nvs_get_str(handle, kNameKey, nullptr, &size) == ESP_OK && size > 1) {
        friendly_name_.resize(size - 1);
        nvs_get_str(handle, kNameKey, friendly_name_.data(), &size);
    } else {
        nvs_set_str(handle, kNameKey, friendly_name_.c_str());
    }
    nvs_commit(handle);
    nvs_close(handle);
    return true;
}

const char* DeviceInfo::firmwareVersion() const {
    const esp_app_desc_t* description = esp_app_get_description();
    return description != nullptr && description->version[0] != '\0' ? description->version : CONFIG_LUDANT_FIRMWARE_VERSION;
}

const char* DeviceInfo::bootVersion() const {
#ifdef CONFIG_LUDANT_BOOT_VERSION
    return CONFIG_LUDANT_BOOT_VERSION;
#else
    return "1.0.0";
#endif
}

uint32_t DeviceInfo::otaMaxImageSize() const {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    return partition == nullptr ? 0 : partition->size;
}

int DeviceInfo::secureVersion() const {
    // Kept in Device Info for protocol compatibility. Ludant releases do not
    // use the eFuse anti-rollback counter, so open builds always report zero.
    return 0;
}

std::string DeviceInfo::json() const {
    const char* ota_authorization = "physical_button";
#if CONFIG_LUDANT_OTA_DEVELOPMENT_WINDOW
    ota_authorization = "physical_button_or_timed_window";
#endif
    return std::string("{\"hardware\":\"ESP32-S3\",\"runtime\":\"esp-idf\",\"secureVersion\":") +
        std::to_string(secureVersion()) +
        ",\"moduleSchemas\":{\"sensor.mpu6050\":1,\"sensor.bme280\":1,\"sensor.soil-moisture\":1,\"actuator.relay\":1},\"firmware\":\"" +
        escapeJson(firmwareVersion()) +
        "\",\"otaProtocol\":2,\"protocolVersion\":2,\"supportsOTA\":true,\"otaCapabilities\":[\"signed\",\"sha256\",\"rollback\",\"sequential_write_with_response\",\"sequential_write_without_response\"],\"otaAuthorization\":\"" + ota_authorization + "\",\"bootVersion\":\"" +
        escapeJson(bootVersion()) +
        "\",\"otaMaxImageSize\":" + std::to_string(otaMaxImageSize()) +
        ",\"deviceId\":\"" + escapeJson(device_id_.c_str()) + "\"}";
}
