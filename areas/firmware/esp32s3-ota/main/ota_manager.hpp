#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
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
    bool isActive() const { return busy_.load(); }

    // All lifecycle changes and firmware bytes enter one ordered queue. The
    // BLE callbacks only copy bounded input and wake the OTA owner task.
    bool enqueueBegin(uint32_t image_size, const char* expected_sha256, const char* version,
                      const char* product, const char* hardware, uint8_t ota_protocol,
                      const char* signature_base64);
    bool enqueueData(const uint8_t* data, size_t length);
    bool enqueueFinish();
    bool enqueueAbort(const char* reason);
    void onDisconnect();

private:
    enum class EventType : uint8_t { Begin, Data, Finish, Abort };
    struct Event {
        EventType type{EventType::Data};
        uint32_t image_size{0};
        uint8_t ota_protocol{0};
        char expected_sha256[65]{};
        char version[33]{};
        char product[32]{};
        char hardware[32]{};
        char signature[129]{};
        char reason[64]{};
        uint16_t length{0};
        uint8_t data[512]{};
    };

    static void dataWorkerTask(void* argument);
    void processEvents();
    void processEvent(const Event& event);
    void processEmergency(uint32_t notification);
    bool begin(uint32_t image_size, const char* expected_sha256, const char* version,
               const char* product, const char* hardware, uint8_t ota_protocol,
               const char* signature_base64);
    bool finish();
    void abort(const char* reason, bool notify = true);
    bool commitData(const uint8_t* data, size_t length);
    void clearState();
    void report(const char* message);
    void reportError(const char* code, const char* description);
    bool validateExpectedHash(const char* expected_sha256) const;
    bool validateVersion(const char* version) const;
    int compareVersions(const char* lhs, const char* rhs) const;
    void recordCurrentVersion();
    bool versionFloor(char* output, size_t capacity) const;
    bool enqueueEvent(const Event& event);
    void queueEmergency(uint32_t notification);

    static constexpr uint8_t kEventQueueCapacity = 10;
    static constexpr uint32_t kEventAvailable = 1U << 0;
    static constexpr uint32_t kQueueFault = 1U << 1;
    static constexpr uint32_t kDisconnect = 1U << 2;

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
    uint32_t data_write_count_{0};
    uint32_t data_write_failures_{0};
    uint32_t last_write_duration_us_{0};
    char expected_sha256_[65]{};
    char version_[33]{};
    char signature_[129]{};
    uint8_t expected_digest_[32]{};
    QueueHandle_t event_queue_{nullptr};
    TaskHandle_t data_worker_{nullptr};
    std::atomic<bool> busy_{false};
};
