#include "module_manager.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

#include "bme280_compensation.hpp"
#include "cJSON.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {
constexpr char TAG[] = "module_manager";
constexpr char kNvsNamespace[] = "ludant";
constexpr char kModulesKey[] = "modules";
constexpr uint32_t kDefaultIntervalMs = 1000;
constexpr std::array<int, 45> kAvailableGpios = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48,
};

bool isAvailableGpio(int gpio) {
    return std::find(kAvailableGpios.begin(), kAvailableGpios.end(), gpio) != kAvailableGpios.end();
}

cJSON* objectItem(cJSON* object, const char* name) {
    return object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, name);
}

const char* stringItem(cJSON* object, const char* name) {
    const cJSON* item = objectItem(object, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

int numberItem(cJSON* object, const char* name, int fallback) {
    const cJSON* item = objectItem(object, name);
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        const char* value = item->valuestring;
        return std::strncmp(value, "0x", 2) == 0
            ? static_cast<int>(std::strtol(value + 2, nullptr, 16))
            : static_cast<int>(std::strtol(value, nullptr, 10));
    }
    return fallback;
}

cJSON* configuration(cJSON* module) { return objectItem(module, "configuration"); }
cJSON* pins(cJSON* module) { return objectItem(configuration(module), "pins"); }
cJSON* parameters(cJSON* module) { return objectItem(configuration(module), "parameters"); }

const char* implementationVersion(const char* plugin) {
    if (plugin == nullptr) return "unknown";
    if (std::strcmp(plugin, "sensor.mpu6050") == 0 ||
        std::strcmp(plugin, "sensor.bme280") == 0 ||
        std::strcmp(plugin, "sensor.soil-moisture") == 0 ||
        std::strcmp(plugin, "actuator.relay") == 0) {
        return "1.0.0";
    }
    return "unknown";
}

void setObjectItem(cJSON* object, const char* name, cJSON* value) {
    if (!cJSON_IsObject(object) || name == nullptr || value == nullptr) {
        cJSON_Delete(value);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(object, name);
    cJSON_AddItemToObject(object, name, value);
}

void annotateModule(cJSON* module) {
    if (!cJSON_IsObject(module)) return;
    const char* plugin = stringItem(module, "pluginId");
    setObjectItem(module, "configurationSchemaVersion", cJSON_CreateNumber(1));
    setObjectItem(module, "implementationVersion", cJSON_CreateString(implementationVersion(plugin)));
}

bool sharesPin(cJSON* first, cJSON* second) {
    cJSON* first_item = nullptr;
    cJSON_ArrayForEach(first_item, first) {
        cJSON* second_item = nullptr;
        cJSON_ArrayForEach(second_item, second) {
            if (cJSON_IsNumber(first_item) && cJSON_IsNumber(second_item) && first_item->valueint == second_item->valueint) return true;
        }
    }
    return false;
}

void addCapability(cJSON* array, const char* driver) {
    cJSON* capability = cJSON_CreateObject();
    cJSON_AddStringToObject(capability, "driverId", driver);
    cJSON_AddStringToObject(capability, "version", "1.0.0");
    cJSON_AddItemToArray(array, capability);
}

int adcChannelForGpio(int gpio) {
    switch (gpio) {
    case 1: return ADC1_CHANNEL_0;
    case 2: return ADC1_CHANNEL_1;
    case 3: return ADC1_CHANNEL_2;
    case 4: return ADC1_CHANNEL_3;
    case 5: return ADC1_CHANNEL_4;
    case 6: return ADC1_CHANNEL_5;
    case 7: return ADC1_CHANNEL_6;
    case 8: return ADC1_CHANNEL_7;
    case 9: return ADC1_CHANNEL_8;
    case 10: return ADC1_CHANNEL_9;
    default: return -1;
    }
}

int16_t signedBME28012(uint16_t value) {
    return static_cast<int16_t>((value & 0x0800) != 0 ? value | 0xF000 : value);
}

void decodeBME280Calibration(const uint8_t* temperature_pressure,
                             const uint8_t* humidity,
                             ludant::BME280Calibration& calibration) {
    auto unsigned16 = [](const uint8_t* bytes, size_t offset) {
        return static_cast<uint16_t>(bytes[offset]) |
               static_cast<uint16_t>(bytes[offset + 1]) << 8;
    };
    auto signed16 = [&unsigned16](const uint8_t* bytes, size_t offset) {
        return static_cast<int16_t>(unsigned16(bytes, offset));
    };
    calibration.dig_t1 = unsigned16(temperature_pressure, 0);
    calibration.dig_t2 = signed16(temperature_pressure, 2);
    calibration.dig_t3 = signed16(temperature_pressure, 4);
    calibration.dig_p1 = unsigned16(temperature_pressure, 6);
    calibration.dig_p2 = signed16(temperature_pressure, 8);
    calibration.dig_p3 = signed16(temperature_pressure, 10);
    calibration.dig_p4 = signed16(temperature_pressure, 12);
    calibration.dig_p5 = signed16(temperature_pressure, 14);
    calibration.dig_p6 = signed16(temperature_pressure, 16);
    calibration.dig_p7 = signed16(temperature_pressure, 18);
    calibration.dig_p8 = signed16(temperature_pressure, 20);
    calibration.dig_p9 = signed16(temperature_pressure, 22);
    calibration.dig_h2 = signed16(humidity, 0);
    calibration.dig_h3 = humidity[2];
    calibration.dig_h4 = signedBME28012(static_cast<uint16_t>(humidity[3]) << 4 | (humidity[4] & 0x0F));
    calibration.dig_h5 = signedBME28012(static_cast<uint16_t>(humidity[5]) << 4 | (humidity[4] >> 4));
    calibration.dig_h6 = static_cast<int8_t>(humidity[6]);
}

} // namespace

ModuleManager::ModuleManager(const DeviceInfo& device_info) : device_info_(device_info) {}

ModuleManager::~ModuleManager() { stopTelemetry(); }

void ModuleManager::setOutputCallback(ModuleOutputCallback callback, void* context) {
    output_callback_ = callback;
    output_context_ = context;
}

void ModuleManager::setTelemetryCallback(ModuleTelemetryCallback callback, void* context) {
    telemetry_callback_ = callback;
    telemetry_context_ = context;
}

bool ModuleManager::begin() {
    return loadPersistedModules();
}

bool ModuleManager::loadPersistedModules() {
    std::string stored;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        size_t size = 0;
        if (nvs_get_str(handle, kModulesKey, nullptr, &size) == ESP_OK && size > 1) {
            stored.assign(size, '\0');
            if (nvs_get_str(handle, kModulesKey, stored.data(), &size) == ESP_OK) {
                stored.resize(std::strlen(stored.c_str()));
            }
        }
        nvs_close(handle);
    }
    if (!stored.empty() && !normalizePersistedModules(stored.c_str())) {
        nvs_handle_t backup_handle;
        if (nvs_open(kNvsNamespace, NVS_READWRITE, &backup_handle) == ESP_OK) {
            nvs_set_str(backup_handle, "modules_backup", stored.c_str());
            nvs_commit(backup_handle);
            nvs_close(backup_handle);
        }
    }
    return true;
}

void ModuleManager::persistModules() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    cJSON* envelope = cJSON_CreateObject();
    cJSON_AddNumberToObject(envelope, "schemaVersion", kStateSchemaVersion);
    cJSON* modules = cJSON_Parse(modules_json_.c_str());
    cJSON_AddItemToObject(envelope, "modules", modules != nullptr && cJSON_IsArray(modules) ? modules : cJSON_CreateArray());
    char* serialized = cJSON_PrintUnformatted(envelope);
    if (serialized != nullptr) {
        nvs_set_str(handle, kModulesKey, serialized);
        cJSON_free(serialized);
    }
    cJSON_Delete(envelope);
    nvs_commit(handle);
    nvs_close(handle);
}

bool ModuleManager::normalizePersistedModules(const char* serialized) {
    cJSON* parsed = cJSON_Parse(serialized);
    if (parsed == nullptr) {
        migration_error_ = "stored module configuration is not valid JSON";
        return false;
    }

    cJSON* modules = parsed;
    bool needsMigration = true;
    if (cJSON_IsObject(parsed)) {
        cJSON* schema = objectItem(parsed, "schemaVersion");
        cJSON* envelopeModules = objectItem(parsed, "modules");
        if (!cJSON_IsNumber(schema) || !cJSON_IsArray(envelopeModules) || schema->valueint < 1 || schema->valueint > kStateSchemaVersion) {
            cJSON_Delete(parsed);
            migration_error_ = "stored module configuration schema is unsupported";
            return false;
        }
        modules = envelopeModules;
        needsMigration = schema->valueint != kStateSchemaVersion;
    }
    if (!cJSON_IsArray(modules)) {
        cJSON_Delete(parsed);
        migration_error_ = "stored module configuration must contain an array";
        return false;
    }

    cJSON* normalized = cJSON_CreateArray();
    cJSON* item = nullptr;
    int rejected = 0;
    cJSON_ArrayForEach(item, modules) {
        const char* error = nullptr;
        if (validateModule(item, &error)) {
            if (!cJSON_IsNumber(objectItem(item, "configurationSchemaVersion")) ||
                !cJSON_IsString(objectItem(item, "implementationVersion"))) {
                needsMigration = true;
            }
            cJSON* copy = cJSON_Duplicate(item, true);
            annotateModule(copy);
            cJSON_AddItemToArray(normalized, copy);
        } else {
            ++rejected;
            // Preserve the original record so a future firmware can migrate
            // it. Disable only this module instead of silently deleting user
            // configuration or clearing the complete module set.
            cJSON* copy = cJSON_Duplicate(item, true);
            if (cJSON_IsObject(copy)) {
                setObjectItem(copy, "enabled", cJSON_CreateBool(false));
                setObjectItem(copy, "migrationError", cJSON_CreateString(error == nullptr ? "unsupported module configuration" : error));
                annotateModule(copy);
                cJSON_AddItemToArray(normalized, copy);
            } else {
                cJSON_Delete(copy);
            }
        }
    }
    char* normalizedText = cJSON_PrintUnformatted(normalized);
    if (normalizedText == nullptr) {
        cJSON_Delete(normalized);
        cJSON_Delete(parsed);
        migration_error_ = "could not normalize stored module configuration";
        return false;
    }
    modules_json_ = normalizedText;
    cJSON_free(normalizedText);
    cJSON_Delete(normalized);
    cJSON_Delete(parsed);
    if (rejected > 0) {
        migration_error_ = "one or more stored modules were disabled because their configuration is no longer supported";
        persistModules();
    } else if (needsMigration) {
        persistModules();
    }
    return true;
}

int ModuleManager::pinValue(void* rawModule, const char* name) const {
    cJSON* module = static_cast<cJSON*>(rawModule);
    return numberItem(pins(module), name, -1);
}

int ModuleManager::parameterValue(void* rawModule, const char* name, int fallback) const {
    return numberItem(parameters(static_cast<cJSON*>(rawModule)), name, fallback);
}

bool ModuleManager::validatePins(void* rawModule, const char** error) const {
    cJSON* module = static_cast<cJSON*>(rawModule);
    cJSON* pin_object = pins(module);
    if (!cJSON_IsObject(pin_object)) { *error = "module pins are required"; return false; }
    std::array<int, 8> seen{};
    size_t seen_count = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, pin_object) {
        if (!cJSON_IsNumber(item) || !isAvailableGpio(item->valueint)) { *error = "one or more GPIOs are not available"; return false; }
        if (std::find(seen.begin(), seen.begin() + seen_count, item->valueint) != seen.begin() + seen_count) { *error = "a GPIO cannot be used twice by one module"; return false; }
        if (seen_count < seen.size()) seen[seen_count++] = item->valueint;
    }
    return true;
}

bool ModuleManager::validateModule(void* rawModule, const char** error) const {
    cJSON* module = static_cast<cJSON*>(rawModule);
    const char* plugin = stringItem(module, "pluginId");
    const char* instance = stringItem(module, "instanceId");
    if (plugin == nullptr || instance == nullptr) { *error = "pluginId and instanceId are required"; return false; }
    if (!validatePins(module, error)) return false;
    cJSON* config = configuration(module);
    if (!cJSON_IsObject(config)) { *error = "module configuration is required"; return false; }
    const int64_t interval = static_cast<int64_t>(numberItem(module, "updateIntervalMs", 0));
    if (interval <= 0) { *error = "updateIntervalMs must be positive"; return false; }
    if (std::strcmp(plugin, "sensor.mpu6050") == 0 || std::strcmp(plugin, "sensor.bme280") == 0) {
        const int address = numberItem(parameters(module), "address", 0);
        if (address < 0x03 || address > 0x77) { *error = "I2C address is outside the supported range"; return false; }
        if (pinValue(module, "sda") < 0 || pinValue(module, "scl") < 0) { *error = "SDA and SCL are required"; return false; }
    } else if (std::strcmp(plugin, "sensor.soil-moisture") == 0 && adcChannelForGpio(pinValue(module, "signal")) < 0) {
        *error = "the selected GPIO does not support analog input"; return false;
    } else if (std::strcmp(plugin, "actuator.relay") != 0 && std::strcmp(plugin, "sensor.soil-moisture") != 0 &&
               std::strcmp(plugin, "sensor.mpu6050") != 0 && std::strcmp(plugin, "sensor.bme280") != 0) {
        *error = "this controller does not have that module driver"; return false;
    }
    return true;
}

bool ModuleManager::validateModuleSet(void* rawModules, const char** error) const {
    cJSON* modules = static_cast<cJSON*>(rawModules);
    if (!cJSON_IsArray(modules)) { *error = "modules must be an array"; return false; }

    for (int index = 0; index < cJSON_GetArraySize(modules); ++index) {
        cJSON* module = cJSON_GetArrayItem(modules, index);
        if (!validateModule(module, error)) return false;
        const char* instance = stringItem(module, "instanceId");
        for (int previous = 0; previous < index; ++previous) {
            cJSON* other = cJSON_GetArrayItem(modules, previous);
            const char* other_instance = stringItem(other, "instanceId");
            if (instance != nullptr && other_instance != nullptr && std::strcmp(instance, other_instance) == 0) {
                *error = "module instance IDs must be unique";
                return false;
            }

            const char* plugin = stringItem(module, "pluginId");
            const char* other_plugin = stringItem(other, "pluginId");
            const bool module_i2c = plugin != nullptr && (std::strcmp(plugin, "sensor.mpu6050") == 0 || std::strcmp(plugin, "sensor.bme280") == 0);
            const bool other_i2c = other_plugin != nullptr && (std::strcmp(other_plugin, "sensor.mpu6050") == 0 || std::strcmp(other_plugin, "sensor.bme280") == 0);
            const bool same_bus = module_i2c && other_i2c &&
                pinValue(module, "sda") == pinValue(other, "sda") && pinValue(module, "scl") == pinValue(other, "scl");
            const bool same_address = same_bus && parameterValue(module, "address", -1) == parameterValue(other, "address", -2);
            if (same_address || (!same_bus && sharesPin(pins(module), pins(other)))) {
                *error = same_address ? "another I2C module already uses this address" : "one or more GPIOs are already in use";
                return false;
            }
        }
    }
    return true;
}

bool ModuleManager::replaceModules(void* rawModules) {
    cJSON* modules = static_cast<cJSON*>(rawModules);
    cJSON* copy = cJSON_Duplicate(modules, true);
    if (copy == nullptr || !cJSON_IsArray(copy)) {
        cJSON_Delete(copy);
        return false;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, copy) annotateModule(item);
    char* serialized = cJSON_PrintUnformatted(copy);
    if (serialized == nullptr) {
        cJSON_Delete(copy);
        return false;
    }
    modules_json_ = serialized;
    cJSON_free(serialized);
    cJSON_ArrayForEach(item, copy) configureRelay(item);
    cJSON_Delete(copy);
    persistModules();
    return true;
}

void ModuleManager::emitResponse(const char* request_id, bool ok, const char* error, void* rawPayload) {
    if (output_callback_ == nullptr) return;
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "type", "response");
    cJSON_AddStringToObject(response, "requestId", request_id == nullptr ? "" : request_id);
    cJSON_AddBoolToObject(response, "ok", ok);
    if (error != nullptr) cJSON_AddStringToObject(response, "error", error);
    if (rawPayload != nullptr) cJSON_AddItemToObject(response, "payload", static_cast<cJSON*>(rawPayload));
    char* output = cJSON_PrintUnformatted(response);
    if (output != nullptr) { output_callback_(output, output_context_); cJSON_free(output); }
    cJSON_Delete(response);
}

void ModuleManager::emitError(const char* request_id, const char* message) { emitResponse(request_id, false, message, nullptr); }

void* ModuleManager::statePayload() const {
    cJSON* payload = cJSON_CreateObject();
    cJSON* state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "deviceId", device_info_.deviceId());
    cJSON_AddStringToObject(state, "friendlyName", device_info_.friendlyName());
    cJSON_AddStringToObject(state, "firmwareVersion", device_info_.firmwareVersion());
    cJSON* capabilities = cJSON_CreateArray();
    addCapability(capabilities, "mpu6050");
    addCapability(capabilities, "bme280");
    addCapability(capabilities, "soil-moisture");
    addCapability(capabilities, "relay");
    cJSON_AddItemToObject(state, "capabilities", capabilities);
    cJSON* modules = cJSON_Parse(modules_json_.c_str());
    cJSON_AddItemToObject(state, "modules", modules == nullptr ? cJSON_CreateArray() : modules);
    cJSON_AddNumberToObject(state, "moduleStateSchemaVersion", kStateSchemaVersion);
    if (!migration_error_.empty()) cJSON_AddStringToObject(state, "moduleMigrationError", migration_error_.c_str());
    cJSON* gpios = cJSON_CreateArray();
    for (int gpio : kAvailableGpios) cJSON_AddItemToArray(gpios, cJSON_CreateNumber(gpio));
    cJSON_AddItemToObject(state, "availableGPIOs", gpios);
    cJSON_AddItemToObject(payload, "state", state);
    return payload;
}

bool ModuleManager::handleCommand(const char* message, size_t length) {
    cJSON* root = cJSON_ParseWithLength(message, length);
    if (root == nullptr) return false;
    const char* command = stringItem(root, "command");
    const char* request_id = stringItem(root, "requestId");
    if (command == nullptr || request_id == nullptr) { cJSON_Delete(root); return false; }

    if (std::strcmp(command, "get_state") == 0) {
        emitResponse(request_id, true, nullptr, statePayload());
    } else if (std::strcmp(command, "apply_modules") == 0) {
        cJSON* modules = objectItem(root, "modules");
        const char* error = nullptr;
        if (!validateModuleSet(modules, &error)) emitError(request_id, error);
        else if (!replaceModules(modules)) emitError(request_id, "could not persist the module configuration");
        else emitResponse(request_id, true, nullptr, statePayload());
    } else if (std::strcmp(command, "configure_module") == 0) {
        cJSON* module = objectItem(root, "module");
        const char* error = nullptr;
        if (!validateModule(module, &error)) emitError(request_id, error);
        else {
            cJSON* modules = cJSON_Parse(modules_json_.c_str());
            bool replaced = false;
            cJSON* item = nullptr;
            const char* new_id = stringItem(module, "instanceId");
            const char* new_plugin = stringItem(module, "pluginId");
            const bool new_i2c = new_plugin != nullptr && (std::strcmp(new_plugin, "sensor.mpu6050") == 0 || std::strcmp(new_plugin, "sensor.bme280") == 0);
            const int new_sda = pinValue(module, "sda");
            const int new_scl = pinValue(module, "scl");
            const int new_address = parameterValue(module, "address", -1);
            for (int index = 0; modules != nullptr && index < cJSON_GetArraySize(modules); ++index) {
                item = cJSON_GetArrayItem(modules, index);
                const char* existing_id = stringItem(item, "instanceId");
                if (existing_id != nullptr && new_id != nullptr && std::strcmp(existing_id, new_id) == 0) {
                    replaced = true;
                    continue;
                }
                const char* existing_plugin = stringItem(item, "pluginId");
                const bool existing_i2c = existing_plugin != nullptr && (std::strcmp(existing_plugin, "sensor.mpu6050") == 0 || std::strcmp(existing_plugin, "sensor.bme280") == 0);
                const bool same_pin = new_sda >= 0 && new_scl >= 0 && new_sda == pinValue(item, "sda") && new_scl == pinValue(item, "scl");
                const bool same_address = new_i2c && existing_i2c && same_pin && new_address == parameterValue(item, "address", -2);
                const bool conflicting_pin = !new_i2c || !existing_i2c || !same_pin;
                if (same_address || (conflicting_pin && sharesPin(pins(module), pins(item)))) {
                    cJSON_Delete(modules);
                    emitError(request_id, same_address ? "another I2C module already uses this address" : "one or more GPIOs are already in use");
                    cJSON_Delete(root);
                    return true;
                }
            }
            if (modules == nullptr) modules = cJSON_CreateArray();
            annotateModule(module);
            if (replaced) {
                for (int index = cJSON_GetArraySize(modules) - 1; index >= 0; --index) {
                    cJSON* existing = cJSON_GetArrayItem(modules, index);
                    const char* existing_id = stringItem(existing, "instanceId");
                    if (existing_id != nullptr && new_id != nullptr && std::strcmp(existing_id, new_id) == 0) cJSON_ReplaceItemInArray(modules, index, cJSON_Duplicate(module, true));
                }
            }
            if (!replaced) cJSON_AddItemToArray(modules, cJSON_Duplicate(module, true));
            char* serialized = cJSON_PrintUnformatted(modules);
            if (serialized != nullptr) { modules_json_ = serialized; cJSON_free(serialized); }
            cJSON_Delete(modules);
            configureRelay(module);
            persistModules();
            emitResponse(request_id, true, nullptr, nullptr);
        }
    } else if (std::strcmp(command, "remove_module") == 0) {
        const char* instance_id = stringItem(root, "instanceId");
        cJSON* modules = cJSON_Parse(modules_json_.c_str());
        for (int index = cJSON_GetArraySize(modules) - 1; index >= 0; --index) {
            cJSON* item = cJSON_GetArrayItem(modules, index);
            const char* existing_id = stringItem(item, "instanceId");
            if (instance_id != nullptr && existing_id != nullptr && std::strcmp(existing_id, instance_id) == 0) cJSON_DeleteItemFromArray(modules, index);
            }
            cJSON_ArrayForEach(item, modules) annotateModule(item);
            char* serialized = cJSON_PrintUnformatted(modules);
        if (serialized != nullptr) { modules_json_ = serialized; cJSON_free(serialized); }
        cJSON_Delete(modules); persistModules(); emitResponse(request_id, true, nullptr, nullptr);
    } else if (std::strcmp(command, "set_module_enabled") == 0) {
        const char* instance_id = stringItem(root, "instanceId");
        cJSON* modules = cJSON_Parse(modules_json_.c_str()); cJSON* item = nullptr;
        cJSON_ArrayForEach(item, modules) {
            const char* existing_id = stringItem(item, "instanceId");
            if (instance_id != nullptr && existing_id != nullptr && std::strcmp(existing_id, instance_id) == 0) {
                cJSON_ReplaceItemInObject(item, "enabled", cJSON_CreateBool(cJSON_IsTrue(objectItem(root, "enabled"))));
            }
        }
        char* serialized = cJSON_PrintUnformatted(modules);
        if (serialized != nullptr) { modules_json_ = serialized; cJSON_free(serialized); }
        cJSON_Delete(modules); persistModules(); emitResponse(request_id, true, nullptr, nullptr);
    } else if (std::strcmp(command, "start_telemetry") == 0 || std::strcmp(command, "set_live_interval") == 0) {
        const char* requested_instance = stringItem(root, "instanceId");
        telemetry_instance_ = requested_instance == nullptr ? "" : requested_instance;
        telemetry_interval_ms_ = std::max<uint32_t>(50, numberItem(root, "intervalMs", kDefaultIntervalMs));
        const char* transport = stringItem(root, "telemetryTransport");
        binary_telemetry_enabled_ = transport != nullptr && std::strcmp(transport, "binary-v2") == 0;
        if (!telemetry_running_) { telemetry_running_ = true; xTaskCreate(telemetryTask, "ludant_telemetry", 4096, this, 4, reinterpret_cast<TaskHandle_t*>(&telemetry_task_)); }
        emitResponse(request_id, true, nullptr, nullptr);
    } else if (std::strcmp(command, "stop_telemetry") == 0) {
        stopTelemetry(); emitResponse(request_id, true, nullptr, nullptr);
    } else {
        emitError(request_id, "unknown controller command");
    }
    cJSON_Delete(root);
    return true;
}

void ModuleManager::stopTelemetry() { telemetry_running_ = false; telemetry_instance_.clear(); binary_telemetry_enabled_ = false; }

void ModuleManager::telemetryTask(void* argument) {
    auto* manager = static_cast<ModuleManager*>(argument);
    while (manager->telemetry_running_) { manager->runTelemetry(); vTaskDelay(pdMS_TO_TICKS(manager->telemetry_interval_ms_)); }
    manager->telemetry_task_ = nullptr;
    vTaskDelete(nullptr);
}

void ModuleManager::runTelemetry() { emitTelemetry(); }

void ModuleManager::emitTelemetry() {
    if (output_callback_ == nullptr && telemetry_callback_ == nullptr) return;
    if (telemetry_instance_.empty()) return;
    cJSON* modules = cJSON_Parse(modules_json_.c_str()); cJSON* module = nullptr;
    cJSON_ArrayForEach(module, modules) {
        const char* existing_id = stringItem(module, "instanceId");
        if (existing_id != nullptr && std::strcmp(existing_id, telemetry_instance_.c_str()) == 0) break;
    }
    if (module == nullptr) { cJSON_Delete(modules); return; }
    if (cJSON_IsFalse(objectItem(module, "enabled"))) { cJSON_Delete(modules); return; }
    const char* plugin = stringItem(module, "pluginId");
    const bool use_binary = binary_telemetry_enabled_ && telemetry_callback_ != nullptr;
    cJSON* values = use_binary ? nullptr : cJSON_CreateObject();
    ludant::BinaryTelemetryPacket binary_packet{};
    binary_packet.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    auto addValue = [&](const char* key, double value) {
        if (use_binary) {
            if (binary_packet.field_count >= ludant::kBinaryTelemetryMaxFields) return;
            auto& field = binary_packet.fields[binary_packet.field_count++];
            std::strncpy(field.name, key, ludant::kBinaryTelemetryFieldNameLength);
            field.name[ludant::kBinaryTelemetryFieldNameLength] = '\0';
            field.value = static_cast<float>(value);
        } else {
            cJSON_AddNumberToObject(values, key, value);
        }
    };
    auto setError = [&](const char* message) {
        if (use_binary && message != nullptr) {
            std::strncpy(binary_packet.error, message, ludant::kBinaryTelemetryErrorLength);
            binary_packet.error[ludant::kBinaryTelemetryErrorLength] = '\0';
        }
    };
    if (std::strcmp(plugin == nullptr ? "" : plugin, "actuator.relay") == 0) addValue("state", parameterValue(module, "state", 0));
    else if (std::strcmp(plugin == nullptr ? "" : plugin, "sensor.soil-moisture") == 0) {
        int channel = adcChannelForGpio(pinValue(module, "signal"));
        int raw = channel < 0 ? 0 : adc1_get_raw(static_cast<adc1_channel_t>(channel));
        int dry = parameterValue(module, "dryValue", 3200); int wet = parameterValue(module, "wetValue", 1400);
        double moisture = dry == wet ? 0.0 : std::clamp(100.0 * (dry - raw) / static_cast<double>(dry - wet), 0.0, 100.0);
        addValue("moisture", moisture);
    } else if (std::strcmp(plugin == nullptr ? "" : plugin, "sensor.mpu6050") == 0) {
        uint8_t bytes[14]{};
        const int address = parameterValue(module, "address", 0x68);
        if (readI2C(pinValue(module, "sda"), pinValue(module, "scl"), address, 0x3B, bytes, sizeof(bytes))) {
            auto signedValue = [&bytes](size_t index) { return static_cast<int16_t>((static_cast<uint16_t>(bytes[index]) << 8) | bytes[index + 1]); };
            const double accel_x = signedValue(0) / 16384.0;
            const double accel_y = signedValue(2) / 16384.0;
            const double accel_z = signedValue(4) / 16384.0;
            const double temperature = signedValue(6) / 340.0 + 36.53;
            const double gyro_x = signedValue(8) / 131.0;
            const double gyro_y = signedValue(10) / 131.0;
            const double gyro_z = signedValue(12) / 131.0;
            addValue("accelX", accel_x);
            addValue("accelY", accel_y);
            addValue("accelZ", accel_z);
            addValue("temperature", temperature);
            addValue("gyroX", gyro_x);
            addValue("gyroY", gyro_y);
            addValue("gyroZ", gyro_z);
        } else {
            setError("MPU6050 read failed");
        }
    } else if (std::strcmp(plugin == nullptr ? "" : plugin, "sensor.bme280") == 0) {
        uint8_t bytes[8]{};
        uint8_t temperature_pressure_calibration[24]{};
        uint8_t humidity_calibration[7]{};
        const int address = parameterValue(module, "address", 0x76);
        uint8_t humidity_one = 0;
        const bool calibration_read =
            readI2C(pinValue(module, "sda"), pinValue(module, "scl"), address, 0x88,
                    temperature_pressure_calibration, sizeof(temperature_pressure_calibration)) &&
            readI2C(pinValue(module, "sda"), pinValue(module, "scl"), address, 0xE1,
                    humidity_calibration, sizeof(humidity_calibration)) &&
            readI2C(pinValue(module, "sda"), pinValue(module, "scl"), address, 0xA1,
                    &humidity_one, 1);
        if (calibration_read &&
            readI2C(pinValue(module, "sda"), pinValue(module, "scl"), address, 0xF7, bytes, sizeof(bytes))) {
            const int pressure = (static_cast<int>(bytes[0]) << 12) | (static_cast<int>(bytes[1]) << 4) | (bytes[2] >> 4);
            const int temperature = (static_cast<int>(bytes[3]) << 12) | (static_cast<int>(bytes[4]) << 4) | (bytes[5] >> 4);
            const int humidity = (static_cast<int>(bytes[6]) << 8) | bytes[7];
            ludant::BME280Calibration calibration;
            decodeBME280Calibration(temperature_pressure_calibration, humidity_calibration, calibration);
            calibration.dig_h1 = humidity_one;
            double temperature_c = 0.0;
            double humidity_percent = 0.0;
            double pressure_hpa = 0.0;
            if (ludant::compensateBME280(calibration, pressure, temperature, humidity,
                                         temperature_c, humidity_percent, pressure_hpa)) {
                addValue("pressure", pressure_hpa);
                addValue("temperature", temperature_c);
                addValue("humidity", humidity_percent);
            } else {
                setError("BME280 compensation failed");
            }
        } else {
            setError("BME280 read failed");
        }
    }
    if (use_binary) {
        binary_packet.sequence = static_cast<uint16_t>(telemetry_sequence_++);
        binary_packet.quality = binary_packet.field_count > 0 && binary_packet.error[0] == '\0' ? 0 : 1;
        if (binary_packet.quality != 0 && binary_packet.error[0] == '\0') {
            setError("No telemetry values available");
        }
        telemetry_callback_(binary_packet, telemetry_context_);
        cJSON_Delete(modules);
        return;
    }
    cJSON* packet = cJSON_CreateObject(); cJSON_AddStringToObject(packet, "type", "telemetry");
    cJSON_AddStringToObject(packet, "deviceId", device_info_.deviceId()); cJSON_AddStringToObject(packet, "instanceId", telemetry_instance_.c_str());
    timeval now{};
    gettimeofday(&now, nullptr);
    cJSON_AddNumberToObject(packet, "timestamp", static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_usec) / 1'000'000.0);
    cJSON_AddItemToObject(packet, "values", values);
    char* output = cJSON_PrintUnformatted(packet);
    if (output != nullptr) { output_callback_(output, output_context_); cJSON_free(output); }
    cJSON_Delete(packet); cJSON_Delete(modules);
}

bool ModuleManager::readI2C(int sda, int scl, int address, uint8_t reg, uint8_t* output, size_t length) const {
    if (output == nullptr || length == 0) return false;
    i2c_config_t config{};
    config.mode = I2C_MODE_MASTER;
    config.sda_io_num = static_cast<gpio_num_t>(sda);
    config.scl_io_num = static_cast<gpio_num_t>(scl);
    config.sda_pullup_en = GPIO_PULLUP_ENABLE;
    config.scl_pullup_en = GPIO_PULLUP_ENABLE;
    config.master.clk_speed = 400000;
    if (i2c_param_config(I2C_NUM_0, &config) != ESP_OK) return false;
    if (i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) return false;
    const esp_err_t result = i2c_master_write_read_device(I2C_NUM_0, static_cast<uint8_t>(address), &reg, 1, output, length, pdMS_TO_TICKS(100));
    i2c_driver_delete(I2C_NUM_0);
    return result == ESP_OK;
}
bool ModuleManager::readModule(void*, void*) const { return false; }

bool ModuleManager::configureRelay(void* rawModule) const {
    cJSON* module = static_cast<cJSON*>(rawModule);
    const char* plugin = stringItem(module, "pluginId");
    if (std::strcmp(plugin == nullptr ? "" : plugin, "actuator.relay") != 0) return true;
    int output = pinValue(module, "output");
    gpio_config_t config{}; config.pin_bit_mask = 1ULL << output; config.mode = GPIO_MODE_OUTPUT; config.pull_down_en = GPIO_PULLDOWN_DISABLE; config.pull_up_en = GPIO_PULLUP_DISABLE; config.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&config) != ESP_OK) return false;
    int state = cJSON_IsFalse(objectItem(module, "enabled")) ? 0 : parameterValue(module, "state", 0);
    int active_high = parameterValue(module, "activeHigh", 1);
    gpio_set_level(static_cast<gpio_num_t>(output), active_high ? state : !state);
    return true;
}
