#include "ble_ota_server.hpp"
#include "device_info.hpp"
#include "ota_manager.hpp"
#include "ota_permission.hpp"
#include "module_manager.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "ludant_main";

constexpr int64_t kNvsRecoveryBootHoldUs = 10LL * 1000LL * 1000LL;

bool confirmNvsRecoveryWithBootButton() {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << CONFIG_LUDANT_BOOT_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_LOGE(TAG, "NVS recovery would erase all saved data and BLE bonds; hold BOOT continuously for 10 seconds to confirm");
    const int64_t started_us = esp_timer_get_time();
    while (esp_timer_get_time() - started_us < kNvsRecoveryBootHoldUs) {
        if (gpio_get_level(static_cast<gpio_num_t>(CONFIG_LUDANT_BOOT_BUTTON_GPIO)) != 0) {
            ESP_LOGE(TAG, "NVS recovery cancelled; saved BLE bonds were preserved");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "Physical BOOT confirmation received for NVS recovery");
    return true;
}

esp_err_t initializeNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS recovery required (%s); automatic erase is disabled to preserve persisted NimBLE bonds",
                 esp_err_to_name(err));
        if (!confirmNvsRecoveryWithBootButton()) {
            return err;
        }
        const esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            return erase_err;
        }
        err = nvs_flash_init();
    }
    return err;
}

void confirmRunningApplication() {
    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    const esp_err_t state_err = esp_ota_get_state_partition(running_partition, &state);
    if (state_err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Running OTA image is pending verification; marking it valid after startup");
        const esp_err_t confirm_err = esp_ota_mark_app_valid_cancel_rollback();
        if (confirm_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not confirm running OTA image: %s", esp_err_to_name(confirm_err));
        }
    }
}
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "Ludant ESP32-S3 firmware starting; version=%s; resetReason=%d",
        CONFIG_LUDANT_FIRMWARE_VERSION, static_cast<int>(esp_reset_reason()));
    const esp_err_t nvs_err = initializeNvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); startup stopped without erasing saved BLE bonds. Hold BOOT for 10 seconds during startup to explicitly recover NVS",
                 esp_err_to_name(nvs_err));
        return;
    }

    // These services are referenced by FreeRTOS tasks and NimBLE callbacks
    // after app_main returns. Keep them in static storage rather than on the
    // app_main task's stack, which ESP-IDF releases when the entry point exits.
    static OtaPermission permission;
    ESP_ERROR_CHECK(permission.begin());

    static DeviceInfo device_info;
    ESP_ERROR_CHECK(device_info.begin() ? ESP_OK : ESP_FAIL);
    static ModuleManager module_manager(device_info);
    ESP_ERROR_CHECK(module_manager.begin() ? ESP_OK : ESP_FAIL);
    static OtaManager ota_manager(permission, device_info);
    static BleOtaServer ble_server(ota_manager, device_info, module_manager);
    ESP_ERROR_CHECK(ble_server.start());
    // The image is confirmed only after the controller, GATT database, and
    // advertising path have initialized successfully. If any of those fail,
    // the bootloader can roll back the pending image.
    confirmRunningApplication();

    ESP_LOGI(TAG, "Firmware ready; version=%s", CONFIG_LUDANT_FIRMWARE_VERSION);
}
