#include <stddef.h>

#include <esp_app_desc.h>

#include "firmware_version.hpp"

namespace {

constexpr void copyAppDescriptionField(char* destination, size_t capacity, const char* source) {
  size_t index = 0;
  for (; index + 1 < capacity && source[index] != '\0'; ++index) destination[index] = source[index];
  destination[index] = '\0';
}

constexpr esp_app_desc_t makeAppDescription() {
  esp_app_desc_t description{};
  description.magic_word = ESP_APP_DESC_MAGIC_WORD;
  copyAppDescriptionField(description.version, sizeof(description.version), LUDANT_FIRMWARE_VERSION);
  copyAppDescriptionField(description.project_name, sizeof(description.project_name), "ludant_esp32s3_ota");
  copyAppDescriptionField(description.time, sizeof(description.time), __TIME__);
  copyAppDescriptionField(description.date, sizeof(description.date), __DATE__);
  return description;
}

}  // namespace

// The Arduino ESP32 core supplies a weak app descriptor whose version is the
// core build revision. Replace it with the release version used by BLE OTA.
extern "C" const esp_app_desc_t esp_app_desc
    __attribute__((section(".rodata_desc"), used)) = makeAppDescription();
