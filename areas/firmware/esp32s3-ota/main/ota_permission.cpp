#include "ota_permission.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr char TAG[] = "ota_permission";
}

esp_err_t OtaPermission::begin() {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << CONFIG_LUDANT_BOOT_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    boot_time_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "OTA permission window open for %d seconds; BOOT button GPIO%d can extend it",
             CONFIG_LUDANT_OTA_WINDOW_SECONDS, CONFIG_LUDANT_BOOT_BUTTON_GPIO);
    return ESP_OK;
}

int64_t OtaPermission::elapsedSinceBootUs() const {
    if (boot_time_us_ == 0) {
        return INT64_MAX;
    }
    return esp_timer_get_time() - boot_time_us_;
}

bool OtaPermission::isAllowed() const {
    const int64_t window_us = static_cast<int64_t>(CONFIG_LUDANT_OTA_WINDOW_SECONDS) * 1000000LL;
    const int64_t elapsed_us = elapsedSinceBootUs();
    const bool within_boot_window = elapsed_us >= 0 && elapsed_us < window_us;
    const bool boot_button_held = gpio_get_level(static_cast<gpio_num_t>(CONFIG_LUDANT_BOOT_BUTTON_GPIO)) == 0;
    return within_boot_window || boot_button_held;
}
