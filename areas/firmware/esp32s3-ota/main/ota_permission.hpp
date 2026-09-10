#pragma once

#include <cstdint>

#include "esp_err.h"

class OtaPermission {
public:
    esp_err_t begin();
    bool isAllowed() const;
    int64_t elapsedSinceBootUs() const;

private:
    int64_t boot_time_us_{0};
};
