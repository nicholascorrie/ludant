#include "ota_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "../shared/ota_auth.hpp"

namespace {
constexpr char TAG[] = "ota_manager";
constexpr uint32_t kProgressInterval = 4U * 1024U;
}

OtaManager::OtaManager(OtaPermission& permission, const DeviceInfo& device_info)
    : permission_(permission), device_info_(device_info) {
    mbedtls_md_init(&sha_context_);
    state_mutex_ = xSemaphoreCreateMutex();
    data_queue_ = xQueueCreate(kDataQueueCapacity, sizeof(DataChunk));
    recordCurrentVersion();
}

bool OtaManager::startDataWorker() {
    if (data_queue_ == nullptr || state_mutex_ == nullptr) return false;
    if (data_worker_ != nullptr) return true;
    if (xTaskCreate(dataWorkerTask, "ludant_ota_data", 4096, this, 5, &data_worker_) != pdPASS) {
        data_worker_ = nullptr;
        ESP_LOGE(TAG, "Could not start OTA data worker");
        return false;
    }
    return true;
}

void OtaManager::dataWorkerTask(void* argument) {
    auto* manager = static_cast<OtaManager*>(argument);
    while (true) manager->processQueuedData();
}

void OtaManager::processQueuedData() {
    if (disconnect_fault_) {
        disconnect_fault_ = false;
        abort("BLE client disconnected", false);
        return;
    }
    if (data_queue_fault_) {
        const char* reason = data_queue_fault_reason_[0] == '\0' ? "OTA data queue fault" : data_queue_fault_reason_;
        data_queue_fault_ = false;
        data_queue_fault_reason_[0] = '\0';
        abort(reason, false);
        reportError("WRITE_FAILED", reason);
        return;
    }
    DataChunk chunk{};
    if (xQueueReceive(data_queue_, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (state_mutex_ != nullptr) xSemaphoreTake(state_mutex_, portMAX_DELAY);
    queued_bytes_ = queued_bytes_ >= chunk.length ? queued_bytes_ - chunk.length : 0;
    data_worker_busy_ = true;
    if (state_mutex_ != nullptr) xSemaphoreGive(state_mutex_);
    commitData(chunk.data, chunk.length);
    if (state_mutex_ != nullptr) xSemaphoreTake(state_mutex_, portMAX_DELAY);
    data_worker_busy_ = false;
    if (state_mutex_ != nullptr) xSemaphoreGive(state_mutex_);
}

bool OtaManager::waitForDataDrain(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        bool drained = data_queue_ == nullptr || uxQueueMessagesWaiting(data_queue_) == 0;
        if (state_mutex_ != nullptr) {
            xSemaphoreTake(state_mutex_, portMAX_DELAY);
            drained = drained && !data_worker_busy_ && queued_bytes_ == 0;
            xSemaphoreGive(state_mutex_);
        }
        if (drained) return true;
        if (static_cast<int32_t>(xTaskGetTickCount() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void OtaManager::setStatusCallback(OtaStatusCallback callback, void* context) {
    status_callback_ = callback;
    status_context_ = context;
}

void OtaManager::report(const char* message) {
    if (status_callback_ != nullptr) {
        status_callback_(message, status_context_);
    }
}

void OtaManager::reportError(const char* code, const char* description) {
    char message[160]{};
    std::snprintf(message, sizeof(message), "ERROR:%s:%s", code, description);
    ESP_LOGE(TAG, "%s", message);
    report(message);
}

bool OtaManager::validateExpectedHash(const char* expected_sha256) const {
    if (expected_sha256 == nullptr || std::strlen(expected_sha256) != 64) {
        return false;
    }

    for (size_t index = 0; index < 64; ++index) {
        const unsigned char character = static_cast<unsigned char>(expected_sha256[index]);
        if (!(std::isdigit(character) || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool OtaManager::validateVersion(const char* version) const {
    if (version == nullptr || version[0] == '\0' || std::strlen(version) >= sizeof(version_)) return false;
    const char* separator = std::strrchr(version, '-');
    const char* suffix = separator == nullptr ? version : separator + 1;
    unsigned major = 0, minor = 0, patch = 0;
    char extra = 0;
    return std::sscanf(suffix, "%u.%u.%u%c", &major, &minor, &patch, &extra) == 3;
}

int OtaManager::compareVersions(const char* lhs, const char* rhs) const {
    auto parse = [](const char* value, unsigned output[3]) {
        const char* separator = std::strrchr(value, '-');
        const char* suffix = separator == nullptr ? value : separator + 1;
        return std::sscanf(suffix, "%u.%u.%u", &output[0], &output[1], &output[2]) == 3;
    };
    unsigned left[3]{}, right[3]{};
    if (!parse(lhs, left) || !parse(rhs, right)) return 0;
    for (size_t index = 0; index < 3; ++index) {
        if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
    }
    return 0;
}

void OtaManager::recordCurrentVersion() {
    nvs_handle_t handle;
    if (nvs_open("ludant", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, "ota_floor", device_info_.firmwareVersion());
    nvs_commit(handle);
    nvs_close(handle);
}

bool OtaManager::versionFloor(char* output, size_t capacity) const {
    if (output == nullptr || capacity == 0) return false;
    nvs_handle_t handle;
    if (nvs_open("ludant", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = capacity;
    const esp_err_t result = nvs_get_str(handle, "ota_floor", output, &length);
    nvs_close(handle);
    return result == ESP_OK;
}

bool OtaManager::begin(uint32_t image_size, const char* expected_sha256, const char* version,
                       const char* product, const char* hardware, uint8_t ota_protocol,
                       const char* signature_base64) {
    if (active_) {
        reportError("ALREADY_ACTIVE", "an OTA update is already in progress");
        return false;
    }
    if (!permission_.isAllowed()) {
        reportError("WINDOW_CLOSED", "OTA is only available during the boot window or with BOOT held");
        return false;
    }
    if (image_size == 0 || !validateExpectedHash(expected_sha256)) {
        reportError("INVALID_BEGIN", "size must be non-zero and sha256 must be 64 lowercase hex characters");
        return false;
    }
    if (!validateVersion(version)) {
        reportError("INVALID_VERSION", "version is required and must be at most 32 characters");
        return false;
    }
    if (std::strcmp(product == nullptr ? "" : product, "ludant-esp32s3") != 0 ||
        std::strcmp(hardware == nullptr ? "" : hardware, "ESP32-S3") != 0 || ota_protocol != 2) {
        reportError("INCOMPATIBLE_ARTIFACT", "product, hardware, or OTA protocol is not supported");
        return false;
    }
    if (signature_base64 == nullptr || std::strlen(signature_base64) >= sizeof(signature_)) {
        reportError("SIGNATURE_INVALID", "a signed firmware artifact is required");
        return false;
    }
    for (size_t index = 0; index < 32; ++index) {
        const char high = expected_sha256[index * 2], low = expected_sha256[index * 2 + 1];
        const auto digit = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        };
        if (digit(high) < 0 || digit(low) < 0) {
            reportError("INVALID_BEGIN", "sha256 must be lowercase hexadecimal");
            return false;
        }
        expected_digest_[index] = static_cast<uint8_t>((digit(high) << 4) | digit(low));
    }
    char floor[sizeof(version_)]{};
    if (versionFloor(floor, sizeof(floor)) && compareVersions(version, floor) <= 0) {
        reportError("VERSION_REJECTED", "firmware version is not newer than the installed version");
        return false;
    }
    if (!ludant::ota_auth::verifyDigestSignature(expected_digest_, signature_base64)) {
        reportError("SIGNATURE_INVALID", "firmware signature verification failed");
        return false;
    }

    const int64_t preparation_started_at = esp_timer_get_time();
    report("PREPARING");
    destination_ = esp_ota_get_next_update_partition(nullptr);
    if (destination_ == nullptr) {
        reportError("NO_PARTITION", "no inactive OTA partition is available");
        return false;
    }
    if (image_size > destination_->size) {
        reportError("IMAGE_TOO_LARGE", "image does not fit in the inactive OTA partition");
        return false;
    }

    esp_err_t err = esp_ota_begin(destination_, image_size, &ota_handle_);
    if (err != ESP_OK) {
        destination_ = nullptr;
        reportError("OTA_BEGIN", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OTA preparation complete: durationMs=%lld imageBytes=%u partition=%s",
             (esp_timer_get_time() - preparation_started_at) / 1000,
             image_size, destination_->label);

    std::strncpy(expected_sha256_, expected_sha256, sizeof(expected_sha256_) - 1);
    std::strncpy(version_, version, sizeof(version_) - 1);
    std::strncpy(signature_, signature_base64, sizeof(signature_) - 1);
    expected_bytes_ = image_size;
    received_bytes_ = 0;
    queued_bytes_ = 0;
    data_queue_fault_ = false;
    disconnect_fault_ = false;
    data_queue_fault_reason_[0] = '\0';
    data_write_count_ = 0;
    data_write_failures_ = 0;
    last_write_duration_us_ = 0;
    if (data_queue_ != nullptr) xQueueReset(data_queue_);
    next_progress_report_ = std::min(image_size, kProgressInterval);
    handle_valid_ = true;
    active_ = true;

    mbedtls_md_free(&sha_context_);
    mbedtls_md_init(&sha_context_);
    if (mbedtls_md_setup(&sha_context_, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 ||
        mbedtls_md_starts(&sha_context_) != 0) {
        abort("SHA initialization failed");
        return false;
    }
    sha_initialized_ = true;

    ESP_LOGI(TAG, "OTA started: destination=%s, size=%u, version=%s",
             destination_->label, expected_bytes_, version_);
    report("READY");
    return true;
}

bool OtaManager::enqueueData(const uint8_t* data, size_t length) {
    if (!active_ || !handle_valid_) {
        reportError("NOT_ACTIVE", "firmware data received without a BEGIN");
        return false;
    }
    if (data == nullptr || length == 0 || length > kDataChunkCapacity ||
        received_bytes_ > expected_bytes_ || queued_bytes_ > expected_bytes_ - received_bytes_ ||
        length > expected_bytes_ - received_bytes_ - queued_bytes_) {
        setDataQueueFault("firmware data exceeded the declared image size");
        return false;
    }
    DataChunk chunk{};
    chunk.length = static_cast<uint16_t>(length);
    std::memcpy(chunk.data, data, length);
    if (data_queue_ == nullptr || xQueueSend(data_queue_, &chunk, 0) != pdTRUE) {
        setDataQueueFault(data_queue_ == nullptr ? "OTA data queue unavailable" : "OTA data queue full");
        return false;
    }
    queued_bytes_ += static_cast<uint32_t>(length);
    ESP_LOGD(TAG, "OTA data queued: chunk=%u received=%u/%u queued=%u depth=%u",
             static_cast<unsigned>(length), received_bytes_, expected_bytes_, queued_bytes_,
             static_cast<unsigned>(uxQueueMessagesWaiting(data_queue_)));
    return true;
}

bool OtaManager::commitData(const uint8_t* data, size_t length) {
    if (!active_ || !handle_valid_) return false;

    const int64_t started_at = esp_timer_get_time();
    const esp_err_t ota_err = esp_ota_write(ota_handle_, data, length);
    last_write_duration_us_ = static_cast<uint32_t>(esp_timer_get_time() - started_at);
    data_write_count_++;
    if (ota_err != ESP_OK) {
        data_write_failures_++;
        ESP_LOGE(TAG, "OTA data write failed: err=%s chunk=%u received=%u/%u queued=%u durationUs=%u",
                 esp_err_to_name(ota_err), static_cast<unsigned>(length), received_bytes_, expected_bytes_,
                 queued_bytes_, last_write_duration_us_);
        abort(esp_err_to_name(ota_err), false);
        reportError("WRITE_FAILED", esp_err_to_name(ota_err));
        return false;
    }
    if (mbedtls_md_update(&sha_context_, data, length) != 0) {
        abort("SHA update failed", false);
        reportError("WRITE_FAILED", "SHA update failed");
        return false;
    }

    received_bytes_ += static_cast<uint32_t>(length);
    ESP_LOGD(TAG, "OTA data committed: chunk=%u received=%u/%u queued=%u depth=%u durationUs=%u writes=%u",
             static_cast<unsigned>(length), received_bytes_, expected_bytes_, queued_bytes_,
             data_queue_ == nullptr ? 0U : static_cast<unsigned>(uxQueueMessagesWaiting(data_queue_)),
             last_write_duration_us_, data_write_count_);
    const bool queue_drained = data_queue_ == nullptr || uxQueueMessagesWaiting(data_queue_) == 0;
    if (received_bytes_ >= next_progress_report_ || queue_drained || received_bytes_ == expected_bytes_) {
        char message[64]{};
        std::snprintf(message, sizeof(message), "PROGRESS:%u:%u",
                      static_cast<unsigned>(received_bytes_),
                      static_cast<unsigned>(expected_bytes_));
        ESP_LOGI(TAG, "%s", message);
        report(message);
        const uint32_t remaining = expected_bytes_ - received_bytes_;
        next_progress_report_ = remaining < kProgressInterval
            ? expected_bytes_
            : received_bytes_ + kProgressInterval;
    }
    return true;
}

void OtaManager::setDataQueueFault(const char* reason) {
    data_queue_fault_ = true;
    std::strncpy(data_queue_fault_reason_, reason == nullptr ? "OTA data queue fault" : reason,
                 sizeof(data_queue_fault_reason_) - 1);
    data_queue_fault_reason_[sizeof(data_queue_fault_reason_) - 1] = '\0';
}

bool OtaManager::finish() {
    if (!active_ || !handle_valid_) {
        reportError("NOT_ACTIVE", "END received without an active OTA update");
        return false;
    }
    if (!waitForDataDrain(30000)) {
        abort("OTA data queue did not drain", false);
        reportError("WRITE_FAILED", "OTA data queue did not drain before END");
        return false;
    }
    if (received_bytes_ != expected_bytes_) {
        abort("received bytes do not match declared image size", false);
        reportError("SIZE_MISMATCH", "received bytes do not match the declared image size");
        return false;
    }

    report("VERIFYING");

    // esp_ota_end validates the ESP application image and releases the OTA handle.
    const esp_ota_handle_t completed_handle = ota_handle_;
    handle_valid_ = false;
    const esp_err_t end_err = esp_ota_end(completed_handle);
    ota_handle_ = 0;
    if (end_err != ESP_OK) {
        clearState();
        reportError("IMAGE_INVALID", esp_err_to_name(end_err));
        return false;
    }

    esp_app_desc_t image_description{};
    if (esp_ota_get_partition_description(destination_, &image_description) != ESP_OK ||
        std::strcmp(image_description.version, version_) != 0) {
        clearState();
        reportError("VERSION_MISMATCH", "requested version does not match the image metadata");
        return false;
    }

    uint8_t digest[32]{};
    if (mbedtls_md_finish(&sha_context_, digest) != 0) {
        clearState();
        reportError("SHA_FINALIZE", "could not finalize SHA-256");
        return false;
    }

    char actual_sha256[65]{};
    for (size_t index = 0; index < sizeof(digest); ++index) {
        std::snprintf(&actual_sha256[index * 2], 3, "%02x", digest[index]);
    }
    if (std::strcmp(actual_sha256, expected_sha256_) != 0) {
        ESP_LOGE(TAG, "SHA mismatch: expected=%s actual=%s", expected_sha256_, actual_sha256);
        clearState();
        reportError("SHA_MISMATCH", "firmware integrity hash does not match");
        return false;
    }

    const esp_err_t boot_err = esp_ota_set_boot_partition(destination_);
    if (boot_err != ESP_OK) {
        clearState();
        reportError("SET_BOOT", esp_err_to_name(boot_err));
        return false;
    }

    ESP_LOGI(TAG, "OTA verified successfully; %s is now the next boot partition", destination_->label);
    active_ = false;
    destination_ = nullptr;
    if (sha_initialized_) {
        mbedtls_md_free(&sha_context_);
        sha_initialized_ = false;
    }
    report("SUCCESS");
    return true;
}

void OtaManager::abort(const char* reason, bool notify) {
    if (!active_ && !handle_valid_) {
        return;
    }

    if (handle_valid_) {
        esp_ota_abort(ota_handle_);
    }
    ESP_LOGW(TAG, "OTA aborted: %s", reason != nullptr ? reason : "unspecified");
    clearState();
    if (notify) {
        report("ABORTED");
    }
}

void OtaManager::onDisconnect() {
    if (active_ || (data_queue_ != nullptr && uxQueueMessagesWaiting(data_queue_) > 0)) {
        // Let the OTA worker perform esp_ota_abort after any in-flight
        // esp_ota_write completes; aborting the handle from the NimBLE task
        // would race the flash write.
        disconnect_fault_ = true;
    }
}

void OtaManager::clearState() {
    ota_handle_ = 0;
    destination_ = nullptr;
    active_ = false;
    handle_valid_ = false;
    expected_bytes_ = 0;
    received_bytes_ = 0;
    next_progress_report_ = 0;
    expected_sha256_[0] = '\0';
    version_[0] = '\0';
    signature_[0] = '\0';
    queued_bytes_ = 0;
    data_queue_fault_ = false;
    disconnect_fault_ = false;
    data_queue_fault_reason_[0] = '\0';
    data_worker_busy_ = false;
    if (data_queue_ != nullptr) xQueueReset(data_queue_);
    std::memset(expected_digest_, 0, sizeof(expected_digest_));
    if (sha_initialized_) {
        mbedtls_md_free(&sha_context_);
        mbedtls_md_init(&sha_context_);
        sha_initialized_ = false;
    }
}
