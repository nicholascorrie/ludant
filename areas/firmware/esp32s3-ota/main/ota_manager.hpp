#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "mbedtls/md.h"

#include "ota_permission.hpp"
#include "device_info.hpp"

using OtaStatusCallback = void (*)(const char* message, void* context);

class OtaManager {
public:
    OtaManager(OtaPermission& permission, const DeviceInfo& device_info);

    void setStatusCallback(OtaStatusCallback callback, void* context);
    bool startDataWorker();
    bool authorizationAllowed() const { return permission_.isAllowed(); }
    bool begin(uint32_t image_size, const char* expected_sha256, const char* version,
               const char* product, const char* hardware, uint8_t ota_protocol,
               const char* signature_base64);
    bool enqueueData(const uint8_t* data, size_t length);
    bool finish();
    void abort(const char* reason, bool notify = true);
    void onDisconnect();

    bool isActive() const { return active_; }
    uint32_t receivedBytes() const { return received_bytes_; }
    uint32_t expectedBytes() const { return expected_bytes_; }

private:
    static void dataWorkerTask(void* argument);
    void processQueuedData();
    bool waitForDataDrain(uint32_t timeout_ms);
    bool commitData(const uint8_t* data, size_t length);
    void setDataQueueFault(const char* reason);
    void clearState();
    void report(const char* message);
    void reportError(const char* code, const char* description);
    bool validateExpectedHash(const char* expected_sha256) const;
    bool validateVersion(const char* version) const;
    int compareVersions(const char* lhs, const char* rhs) const;
    void recordCurrentVersion();
    bool versionFloor(char* output, size_t capacity) const;

    OtaPermission& permission_;
    const DeviceInfo& device_info_;
    OtaStatusCallback status_callback_{nullptr};
    void* status_context_{nullptr};
    esp_ota_handle_t ota_handle_{0};
    const esp_partition_t* destination_{nullptr};
    mbedtls_md_context_t sha_context_{};
    bool sha_initialized_{false};
    bool handle_valid_{false};
    bool active_{false};
    uint32_t expected_bytes_{0};
    uint32_t received_bytes_{0};
    uint32_t next_progress_report_{0};
    char expected_sha256_[65]{};
    char version_[33]{};
    char signature_[129]{};
    uint8_t expected_digest_[32]{};
    static constexpr size_t kDataChunkCapacity = 512;
    static constexpr uint8_t kDataQueueCapacity = 8;
    struct DataChunk {
        uint8_t data[kDataChunkCapacity]{};
        uint16_t length{0};
    };
    QueueHandle_t data_queue_{nullptr};
    SemaphoreHandle_t state_mutex_{nullptr};
    TaskHandle_t data_worker_{nullptr};
    uint32_t queued_bytes_{0};
    uint32_t data_write_count_{0};
    uint32_t data_write_failures_{0};
    uint32_t last_write_duration_us_{0};
    bool data_worker_busy_{false};
    bool data_queue_fault_{false};
    bool disconnect_fault_{false};
    char data_queue_fault_reason_[64]{};
};
