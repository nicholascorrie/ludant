#include "ble_ota_server.hpp"
#include "device_info.hpp"
#include "ota_manager.hpp"
#include "ota_permission.hpp"
#include "module_manager.hpp"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs_flash.h"

namespace {
constexpr char TAG[] = "ludant_main";

void initializeNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
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
    ESP_LOGI(TAG, "Ludant ESP32-S3 firmware starting");
    initializeNvs();
    confirmRunningApplication();

    OtaPermission permission;
    ESP_ERROR_CHECK(permission.begin());

    DeviceInfo device_info;
    ESP_ERROR_CHECK(device_info.begin() ? ESP_OK : ESP_FAIL);
    ModuleManager module_manager(device_info);
    ESP_ERROR_CHECK(module_manager.begin() ? ESP_OK : ESP_FAIL);
    OtaManager ota_manager(permission);
    BleOtaServer ble_server(ota_manager, device_info, module_manager);
    ESP_ERROR_CHECK(ble_server.start());

    ESP_LOGI(TAG, "Firmware ready; normal application logic can be added alongside the OTA layer");
}
