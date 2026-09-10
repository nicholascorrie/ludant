#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "device_info.hpp"

using ModuleOutputCallback = void (*)(const char* message, void* context);

class ModuleManager {
public:
    explicit ModuleManager(const DeviceInfo& device_info);
    ~ModuleManager();

    void setOutputCallback(ModuleOutputCallback callback, void* context);
    bool begin();
    bool handleCommand(const char* message, size_t length);
    void stopTelemetry();

    const char* modulesJson() const { return modules_json_.c_str(); }

private:
    static void telemetryTask(void* argument);
    void runTelemetry();
    void emitResponse(const char* request_id, bool ok, const char* error, void* payload);
    void emitError(const char* request_id, const char* message);
    void emitTelemetry();
    bool validateModule(void* module, const char** error) const;
    bool validateModuleSet(void* modules, const char** error) const;
    bool replaceModules(void* modules);
    void* statePayload() const;
    bool validatePins(void* module, const char** error) const;
    bool readModule(void* module, void* values) const;
    bool readI2C(int sda, int scl, int address, uint8_t reg, uint8_t* output, size_t length) const;
    bool configureRelay(void* module) const;
    int pinValue(void* module, const char* name) const;
    int parameterValue(void* module, const char* name, int fallback) const;
    void persistModules();

    ModuleOutputCallback output_callback_{nullptr};
    void* output_context_{nullptr};
    const DeviceInfo& device_info_;
    std::string modules_json_{"[]"};
    std::string telemetry_instance_;
    uint32_t telemetry_interval_ms_{1000};
    void* telemetry_task_{nullptr};
    bool telemetry_running_{false};
};
