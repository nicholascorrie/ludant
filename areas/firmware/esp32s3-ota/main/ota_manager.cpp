#include "ota_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_log.h"

namespace {
constexpr char TAG[] = "ota_manager";
constexpr uint32_t kProgressInterval = 32U * 1024U;
}

OtaManager::OtaManager(OtaPermission& permission) : permission_(permission) {
    mbedtls_sha256_init(&sha_context_);
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

bool OtaManager::begin(uint32_t image_size, const char* expected_sha256, const char* version) {
    if (!permission_.isAllowed()) {
        reportError("WINDOW_CLOSED", "OTA is only available during the boot window or with BOOT held");
        return false;
    }
    if (active_) {
        reportError("ALREADY_ACTIVE", "an OTA update is already in progress");
        return false;
    }
    if (image_size == 0 || !validateExpectedHash(expected_sha256)) {
        reportError("INVALID_BEGIN", "size must be non-zero and sha256 must be 64 lowercase hex characters");
        return false;
    }
    if (version == nullptr || version[0] == '\0' || std::strlen(version) >= sizeof(version_)) {
        reportError("INVALID_VERSION", "version is required and must be at most 32 characters");
        return false;
    }

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

    std::strncpy(expected_sha256_, expected_sha256, sizeof(expected_sha256_) - 1);
    std::strncpy(version_, version, sizeof(version_) - 1);
    expected_bytes_ = image_size;
    received_bytes_ = 0;
    next_progress_report_ = std::min(image_size, kProgressInterval);
    handle_valid_ = true;
    active_ = true;

    mbedtls_sha256_free(&sha_context_);
    mbedtls_sha256_init(&sha_context_);
    if (mbedtls_sha256_starts_ret(&sha_context_, 0) != 0) {
        abort("SHA initialization failed");
        return false;
    }
    sha_initialized_ = true;

    ESP_LOGI(TAG, "OTA started: destination=%s, size=%u, version=%s",
             destination_->label, expected_bytes_, version_);
    report("READY");
    return true;
}

bool OtaManager::writeData(const uint8_t* data, size_t length) {
    if (!active_ || !handle_valid_) {
        reportError("NOT_ACTIVE", "firmware data received without a BEGIN");
        return false;
    }
    if (data == nullptr || length == 0 || length > expected_bytes_ - received_bytes_) {
        abort("invalid firmware data length");
        return false;
    }

    const esp_err_t ota_err = esp_ota_write(ota_handle_, data, length);
    if (ota_err != ESP_OK) {
        abort(esp_err_to_name(ota_err));
        return false;
    }
    if (mbedtls_sha256_update_ret(&sha_context_, data, length) != 0) {
        abort("SHA update failed");
        return false;
    }

    received_bytes_ += static_cast<uint32_t>(length);
    if (received_bytes_ >= next_progress_report_ || received_bytes_ == expected_bytes_) {
        char message[64]{};
        std::snprintf(message, sizeof(message), "PROGRESS:%u:%u", received_bytes_, expected_bytes_);
        ESP_LOGI(TAG, "%s", message);
        report(message);
        const uint32_t remaining = expected_bytes_ - received_bytes_;
        next_progress_report_ = remaining < kProgressInterval
            ? expected_bytes_
            : received_bytes_ + kProgressInterval;
    }
    return true;
}

bool OtaManager::finish() {
    if (!active_ || !handle_valid_) {
        reportError("NOT_ACTIVE", "END received without an active OTA update");
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

    uint8_t digest[32]{};
    if (mbedtls_sha256_finish_ret(&sha_context_, digest) != 0) {
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
        mbedtls_sha256_free(&sha_context_);
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
    if (active_) {
        abort("BLE client disconnected", false);
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
    if (sha_initialized_) {
        mbedtls_sha256_free(&sha_context_);
        mbedtls_sha256_init(&sha_context_);
        sha_initialized_ = false;
    }
}
