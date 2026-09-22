#pragma once

#include <cstdint>
#include <string>

class DeviceInfo {
public:
    bool begin();
    std::string json() const;
    const char* deviceId() const { return device_id_.c_str(); }
    const char* friendlyName() const { return friendly_name_.c_str(); }
    const char* firmwareVersion() const;
    const char* bootVersion() const;
    uint32_t otaMaxImageSize() const;
    int secureVersion() const;

private:
    std::string device_id_;
    std::string friendly_name_{"Ludant controller"};
};
