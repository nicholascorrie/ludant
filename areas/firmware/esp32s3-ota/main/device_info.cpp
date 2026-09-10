#include "device_info.hpp"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"

namespace {
constexpr char kNamespace[] = "ludant";
constexpr char kDeviceIdKey[] = "device_id";
constexpr char kNameKey[] = "friendly_name";
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

std::string DeviceInfo::json() const {
    char buffer[256]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "{\"device\":\"electronics-controller\",\"chip\":\"ESP32-S3\",\"firmware\":\"%s\",\"otaProtocol\":1,\"supportsModules\":true,\"deviceId\":\"%s\",\"friendlyName\":\"%s\"}",
        firmwareVersion(), device_id_.c_str(), friendly_name_.c_str());
    return buffer;
}
