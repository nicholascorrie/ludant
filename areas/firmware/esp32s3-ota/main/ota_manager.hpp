#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#include "ota_permission.hpp"

using OtaStatusCallback = void (*)(const char* message, void* context);

class OtaManager {
public:
    explicit OtaManager(OtaPermission& permission);

    void setStatusCallback(OtaStatusCallback callback, void* context);
    bool begin(uint32_t image_size, const char* expected_sha256, const char* version);
    bool writeData(const uint8_t* data, size_t length);
    bool finish();
    void abort(const char* reason, bool notify = true);
    void onDisconnect();

    bool isActive() const { return active_; }
    uint32_t receivedBytes() const { return received_bytes_; }
    uint32_t expectedBytes() const { return expected_bytes_; }

private:
    void clearState();
    void report(const char* message);
    void reportError(const char* code, const char* description);
    bool validateExpectedHash(const char* expected_sha256) const;

    OtaPermission& permission_;
    OtaStatusCallback status_callback_{nullptr};
    void* status_context_{nullptr};
    esp_ota_handle_t ota_handle_{0};
    const esp_partition_t* destination_{nullptr};
    mbedtls_sha256_context sha_context_{};
    bool sha_initialized_{false};
    bool handle_valid_{false};
    bool active_{false};
    uint32_t expected_bytes_{0};
    uint32_t received_bytes_{0};
    uint32_t next_progress_report_{0};
    char expected_sha256_[65]{};
    char version_[33]{};
};
