/*
 * Ludant ESP32-S3 module firmware for Arduino IDE.
 *
 * Upload this sketch directly from Arduino IDE. It is the complete
 * module-capable runtime: BLE control, persistent module configuration,
 * GPIO/I2C validation, OTA compatibility, and live telemetry are all kept in
 * this single sketch so no second firmware image is required.
 *
 * Required Arduino ESP32 core: 2.x or newer.
 */

#include <Arduino.h>
#include <ctype.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <mbedtls/sha256.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "../../main/bme280_compensation.hpp"
#include "../../shared/ota_auth.hpp"
#include "../../shared/telemetry_frame.hpp"
#include "firmware_version.hpp"

static constexpr char SERVICE_UUID[] = "7A910000-4C5E-4A9B-8F23-91F4A7D10000";
static constexpr char CONTROL_UUID[] = "7A910001-4C5E-4A9B-8F23-91F4A7D10000";
static constexpr char DATA_UUID[] = "7A910002-4C5E-4A9B-8F23-91F4A7D10000";
static constexpr char STATUS_UUID[] = "7A910003-4C5E-4A9B-8F23-91F4A7D10000";
static constexpr char DEVICE_INFO_UUID[] = "7A910004-4C5E-4A9B-8F23-91F4A7D10000";
static constexpr char TELEMETRY_UUID[] = "7A910005-4C5E-4A9B-8F23-91F4A7D10000";
static constexpr char FIRMWARE_VERSION[] = LUDANT_FIRMWARE_VERSION;
static constexpr uint8_t PROTOCOL_VERSION = 2;
static constexpr uint8_t OTA_PROTOCOL_VERSION = 2;
static constexpr char HARDWARE_TARGET[] = "ESP32-S3";
static constexpr uint8_t BOOT_BUTTON_GPIO = 0;
static constexpr uint8_t MAX_MODULES = 8;
static constexpr uint8_t MAX_FIELDS = 8;
static constexpr uint8_t COMMAND_FRAME_MAGIC = 0xC2;
static constexpr uint8_t COMMAND_FRAME_VERSION = 1;
static constexpr size_t COMMAND_FRAME_HEADER_LENGTH = 12;
static constexpr size_t OTA_DATA_CHUNK_CAPACITY = 512;
static constexpr uint8_t OTA_DATA_QUEUE_CAPACITY = 8;

BLECharacteristic* statusCharacteristic = nullptr;
BLECharacteristic* deviceInfoCharacteristic = nullptr;
BLECharacteristic* telemetryCharacteristic = nullptr;
Preferences preferences;

// Controller state responses can be larger than a single BLE attribute and
// are fragmented by processStatusQueue(). Keep enough room for the complete
// JSON message before fragmentation, rather than silently dropping it.
static constexpr size_t MAX_STATUS_LENGTH = 2048;
// A telemetry packet is currently split into 20-byte BLE notifications. Send
// a small batch per loop so a 300-byte MPU6050 packet can complete within a
// 500 ms cadence without flooding the controller's BLE stack.
static constexpr uint8_t STATUS_FRAMES_PER_LOOP = 3;
static constexpr size_t BINARY_TELEMETRY_MAX_PAYLOAD = ludant::kBinaryTelemetryMaxPayloadLength;
using ludant::BinaryTelemetryPacket;
struct OutboundStatus {
  char data[MAX_STATUS_LENGTH];
};
struct InboundCommand {
  char data[MAX_STATUS_LENGTH];
};
struct ActiveStatus {
  char data[MAX_STATUS_LENGTH];
  uint16_t length = 0;
  uint16_t nextChunk = 0;
  uint16_t totalChunks = 0;
  uint16_t messageId = 0;
  uint16_t checksum = 0;
  uint32_t updatedAt = 0;
  bool active = false;
};
struct ActiveCommand {
  char data[MAX_STATUS_LENGTH];
  uint16_t length = 0;
  uint16_t messageLength = 0;
  uint16_t nextChunk = 0;
  uint16_t totalChunks = 0;
  uint16_t messageId = 0;
  uint16_t checksum = 0;
  uint32_t updatedAt = 0;
  bool active = false;
};
QueueHandle_t responseQueue = nullptr;
QueueHandle_t telemetryQueue = nullptr;
QueueHandle_t binaryTelemetryQueue = nullptr;
QueueHandle_t controlQueue = nullptr;
QueueHandle_t otaDataQueue = nullptr;
ActiveStatus activeStatus;
ActiveCommand activeCommand;
uint16_t statusMessageId = 0;
volatile bool clientConnected = false;
bool binaryTelemetryEnabled = false;
volatile uint32_t binaryTelemetryDroppedCount = 0;
uint32_t lastTelemetryQueueLogAt = 0;
uint32_t lastTelemetryTransmissionLogAt = 0;
uint32_t lastDisconnectedQueueLogAt = 0;
uint32_t lastStatusBlockedLogAt = 0;
uint32_t lastHeartbeatAt = 0;
// Exposed through device-info so diagnostics remain available when the USB
// CDC monitor is attached to a different ESP32-S3 CDC interface.
volatile uint32_t controlWriteCount = 0;
volatile uint32_t commandQueuedCount = 0;
volatile uint32_t commandProcessedCount = 0;
volatile uint32_t responseProducedCount = 0;
volatile uint32_t statusQueuedCount = 0;
volatile uint32_t statusNotifyCount = 0;
volatile uint32_t statusDroppedCount = 0;
volatile uint32_t statusBlockedNoQueueCount = 0;
volatile uint32_t statusBlockedNoCharacteristicCount = 0;
volatile uint32_t statusBlockedNoClientCount = 0;
volatile uint32_t statusTransmissionStartedCount = 0;
volatile uint32_t statusTooLargeCount = 0;
volatile uint32_t statusAllocationFailedCount = 0;
volatile uint32_t lastResponseBytes = 0;
volatile uint32_t lastStatusBytes = 0;
volatile uint32_t lastCommandAt = 0;
volatile uint32_t lastStatusAt = 0;
volatile uint32_t bleConnectCount = 0;
volatile uint32_t bleDisconnectCount = 0;
volatile uint32_t lastBleConnectAt = 0;
volatile uint32_t lastBleDisconnectAt = 0;
volatile uint32_t lastControlWriteAt = 0;
volatile uint32_t lastResponseAt = 0;
uint32_t lastIgnoredDataLogAt = 0;
uint32_t lastDataLogAt = 0;
volatile bool otaDataQueueFault = false;
char otaDataQueueFaultMessage[64] = {};
uint32_t otaQueuedBytes = 0;
uint32_t otaDataWriteCount = 0;
uint32_t otaDataWriteFailures = 0;
uint32_t otaLastWriteDurationUs = 0;

struct OtaDataChunk {
  uint8_t data[OTA_DATA_CHUNK_CAPACITY];
  uint16_t length = 0;
};

void notifyStatus(const String& status);

struct FieldValue {
  String key;
  String value;
};

struct ModuleRecord {
  String instanceId;
  String pluginId;
  String friendlyName;
  String status;
  FieldValue pins[MAX_FIELDS];
  FieldValue parameters[MAX_FIELDS];
  uint8_t pinCount = 0;
  uint8_t parameterCount = 0;
  uint32_t updateIntervalMs = 1000;
  bool enabled = true;
};

bool isTelemetryStatus(const String& status) {
  return status.indexOf("\"type\":\"telemetry\"") >= 0;
}

void logQueueState(const char* event) {
  const unsigned controlWaiting = controlQueue == nullptr ? 0 : uxQueueMessagesWaiting(controlQueue);
  const unsigned controlSpace = controlQueue == nullptr ? 0 : uxQueueSpacesAvailable(controlQueue);
  const unsigned responseWaiting = responseQueue == nullptr ? 0 : uxQueueMessagesWaiting(responseQueue);
  const unsigned responseSpace = responseQueue == nullptr ? 0 : uxQueueSpacesAvailable(responseQueue);
  const unsigned telemetryWaiting = telemetryQueue == nullptr ? 0 : uxQueueMessagesWaiting(telemetryQueue);
  const unsigned telemetrySpace = telemetryQueue == nullptr ? 0 : uxQueueSpacesAvailable(telemetryQueue);
  Serial.printf("Ludant: %s | connected=%d controlQueue=%u waiting/%u free responseQueue=%u waiting/%u free telemetryQueue=%u waiting/%u free ble=%u/%u writes=%u processed=%u responses=%u notified=%u dropped=%u blocked=%u/%u/%u heap=%u\n",
                event, clientConnected ? 1 : 0, controlWaiting, controlSpace,
                responseWaiting, responseSpace, telemetryWaiting, telemetrySpace,
                bleConnectCount, bleDisconnectCount,
                controlWriteCount, commandProcessedCount, responseProducedCount,
                statusNotifyCount, statusDroppedCount,
                statusBlockedNoQueueCount, statusBlockedNoCharacteristicCount,
                statusBlockedNoClientCount, ESP.getFreeHeap());
}

String bleHexPreview(const uint8_t* data, size_t length, size_t limit = 24) {
  String result;
  const size_t count = min(length, limit);
  for (size_t index = 0; index < count; ++index) {
    if (index > 0) result += ' ';
    char byteHex[4] = {};
    snprintf(byteHex, sizeof(byteHex), "%02X", data[index]);
    result += byteHex;
  }
  if (length > limit) result += " ...";
  return result;
}

String bleAsciiPreview(const uint8_t* data, size_t length, size_t limit = 48) {
  String result;
  const size_t count = min(length, limit);
  for (size_t index = 0; index < count; ++index) {
    const char value = static_cast<char>(data[index]);
    result += isprint(static_cast<unsigned char>(value)) ? value : '.';
  }
  if (length > limit) result += "...";
  return result;
}

void logBlePayload(const char* direction, const uint8_t* data, size_t length) {
  Serial.printf("Ludant: BLE %s; bytes=%u; ascii=\"%s\"; hex=%s\n",
                direction, length, bleAsciiPreview(data, length).c_str(),
                bleHexPreview(data, length).c_str());
}

ModuleRecord modules[MAX_MODULES];
// Setup and queued module commands run serially on loopTask. Keep their
// candidate configuration separate from the live modules, but off the 8 KB
// task stack: eight ModuleRecords alone occupy 4,704 bytes on ESP32-S3.
static ModuleRecord candidateModules[MAX_MODULES];
static ModuleRecord configuredModule;
uint8_t moduleCount = 0;
String deviceId;
uint64_t configRevision = 0;
String configHash;
uint8_t configSlot = 0;

volatile bool otaActive = false;
esp_ota_handle_t otaHandle = 0;
const esp_partition_t* otaDestination = nullptr;
bool otaHandleValid = false;
uint32_t expectedSize = 0;
uint32_t receivedSize = 0;
String expectedHash;
String expectedVersion;
String expectedSignature;
mbedtls_sha256_context shaContext;
bool shaActive = false;
uint32_t otaBootStartedAt = 0;

bool telemetryRunning = false;
String telemetryInstance;
uint32_t telemetryIntervalMs = 1000;
uint32_t telemetryNextAt = 0;
uint32_t telemetryLastErrorLogAt = 0;
uint32_t telemetrySequence = 0;
int activeI2CSda = -1;
int activeI2CScl = -1;
struct BME280CalibrationCache {
  bool valid = false;
  int sda = -1;
  int scl = -1;
  int address = -1;
  ludant::BME280Calibration value;
};
BME280CalibrationCache bme280CalibrationCache;

uint16_t crc16(const char* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t index = 0; index < length; ++index) {
    crc ^= static_cast<uint8_t>(data[index]);
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

uint16_t crc16Bytes(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

uint16_t readLittleEndian16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

String base36(uint32_t value, uint8_t width) {
  const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char output[8] = {};
  for (int index = width - 1; index >= 0; --index) {
    output[index] = digits[value % 36];
    value /= 36;
  }
  return String(output);
}

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char character = value[i];
    if (character == '\\' || character == '"') escaped += '\\';
    if (character == '\n') escaped += "\\n";
    else if (character == '\r') escaped += "\\r";
    else escaped += character;
  }
  return escaped;
}

bool otaPermissionAllowed() {
#ifdef LUDANT_OTA_DEVELOPMENT_WINDOW_SECONDS
  const bool withinDevelopmentWindow = millis() - otaBootStartedAt < static_cast<uint32_t>(LUDANT_OTA_DEVELOPMENT_WINDOW_SECONDS) * 1000U;
#else
  const bool withinDevelopmentWindow = false;
#endif
  const bool bootHeld = digitalRead(BOOT_BUTTON_GPIO) == LOW;
  return withinDevelopmentWindow || bootHeld;
}

bool validFirmwareVersion(const String& version) {
  const int separator = version.lastIndexOf('-');
  const String suffix = separator >= 0 ? version.substring(separator + 1) : version;
  int first = suffix.indexOf('.');
  int second = first < 0 ? -1 : suffix.indexOf('.', first + 1);
  if (first <= 0 || second <= first + 1 || second >= static_cast<int>(suffix.length()) - 1) return false;
  for (size_t index = 0; index < suffix.length(); ++index) {
    const char value = suffix[index];
    if (value == '.') continue;
    if (value < '0' || value > '9') return false;
  }
  return true;
}

int compareFirmwareVersions(const String& lhs, const String& rhs) {
  auto component = [](const String& value, uint8_t index) -> uint32_t {
    const int separator = value.lastIndexOf('-');
    const String suffix = value.substring(separator < 0 ? 0 : separator + 1);
    int start = 0;
    for (uint8_t current = 0; current < index; ++current) {
      start = suffix.indexOf('.', start) + 1;
    }
    int end = suffix.indexOf('.', start);
    if (end < 0) end = suffix.length();
    return static_cast<uint32_t>(suffix.substring(start, end).toInt());
  };
  for (uint8_t index = 0; index < 3; ++index) {
    const uint32_t left = component(lhs, index), right = component(rhs, index);
    if (left != right) return left < right ? -1 : 1;
  }
  return 0;
}

bool decodeExpectedHash(const String& hash, uint8_t output[32]) {
  if (hash.length() != 64) return false;
  auto digit = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  };
  for (uint8_t index = 0; index < 32; ++index) {
    const int high = digit(hash[index * 2]), low = digit(hash[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

void clearOtaState(bool abortHandle) {
  if (abortHandle && otaHandleValid) {
    esp_ota_abort(otaHandle);
  }
  otaHandle = 0;
  otaHandleValid = false;
  otaDestination = nullptr;
  if (shaActive) {
    mbedtls_sha256_free(&shaContext);
    shaActive = false;
  }
  otaActive = false;
  if (otaDataQueue != nullptr) xQueueReset(otaDataQueue);
  otaQueuedBytes = 0;
  expectedSize = 0;
  receivedSize = 0;
  expectedHash = "";
  expectedVersion = "";
  expectedSignature = "";
}

void failQueuedOtaWrite(const char* reason) {
  otaDataWriteFailures++;
  Serial.printf("Ludant: OTA data write failed; reason=%s queued=%u received=%u/%u lastWriteUs=%u\n",
                reason, otaQueuedBytes, receivedSize, expectedSize, otaLastWriteDurationUs);
  clearOtaState(true);
  notifyStatus(String("ERROR:WRITE_FAILED:") + reason);
}

void processOtaDataQueue() {
  if (otaDataQueueFault) {
    otaDataQueueFault = false;
    const String reason = otaDataQueueFaultMessage[0] == '\0' ? "OTA data queue full" : otaDataQueueFaultMessage;
    otaDataQueueFaultMessage[0] = '\0';
    failQueuedOtaWrite(reason.c_str());
    return;
  }
  if (otaDataQueue == nullptr || !otaActive || !otaHandleValid) return;

  OtaDataChunk chunk = {};
  if (xQueueReceive(otaDataQueue, &chunk, 0) != pdTRUE) return;
  otaQueuedBytes = otaQueuedBytes >= chunk.length ? otaQueuedBytes - chunk.length : 0;
  const uint32_t startedAt = micros();
  const esp_err_t otaResult = esp_ota_write(otaHandle, chunk.data, chunk.length);
  otaLastWriteDurationUs = micros() - startedAt;
  otaDataWriteCount++;
  if (otaResult != ESP_OK) {
    failQueuedOtaWrite(esp_err_to_name(otaResult));
    return;
  }
  if (shaActive && mbedtls_sha256_update(&shaContext, chunk.data, chunk.length) != 0) {
    failQueuedOtaWrite("SHA-256 update failed");
    return;
  }
  receivedSize += chunk.length;
  if (lastDataLogAt == 0 || millis() - lastDataLogAt >= 1000 || receivedSize >= expectedSize) {
    lastDataLogAt = millis();
    Serial.printf("Ludant: OTA data committed; chunkBytes=%u received=%u/%u queueBytes=%u writeUs=%u writes=%u\n",
                  chunk.length, receivedSize, expectedSize, otaQueuedBytes,
                  otaLastWriteDurationUs, otaDataWriteCount);
  }
  const bool otaQueueDrained = otaDataQueue == nullptr || uxQueueMessagesWaiting(otaDataQueue) == 0;
  if (expectedSize > 0 && (receivedSize == expectedSize || otaQueueDrained || receivedSize % (4U * 1024U) < chunk.length)) {
    notifyStatus(String("PROGRESS:") + String(receivedSize) + ":" + String(expectedSize));
  }
}

void drainOtaDataQueue() {
  while (otaActive && otaDataQueue != nullptr && uxQueueMessagesWaiting(otaDataQueue) > 0) {
    processOtaDataQueue();
  }
}

int skipWhitespace(const String& json, int position) {
  while (position < static_cast<int>(json.length()) && isspace(json[position])) ++position;
  return position;
}

bool readJsonValue(const String& json, int& position, String& value, bool& quoted) {
  position = skipWhitespace(json, position);
  if (position >= static_cast<int>(json.length())) return false;
  quoted = json[position] == '"';
  if (quoted) {
    ++position;
    value = "";
    while (position < static_cast<int>(json.length())) {
      const char character = json[position++];
      if (character == '"') return true;
      if (character == '\\' && position < static_cast<int>(json.length())) {
        const char escaped = json[position++];
        value += escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : escaped;
      } else {
        value += character;
      }
    }
    return false;
  }

  const int start = position;
  while (position < static_cast<int>(json.length()) && json[position] != ',' &&
         json[position] != '}' && json[position] != ']' && !isspace(json[position])) {
    ++position;
  }
  value = json.substring(start, position);
  return value.length() > 0;
}

int keyValueStart(const String& json, const char* key) {
  const String marker = String("\"") + key + "\"";
  const int keyPosition = json.indexOf(marker);
  if (keyPosition < 0) return -1;
  const int colon = json.indexOf(':', keyPosition + marker.length());
  return colon < 0 ? -1 : colon + 1;
}

String jsonValue(const String& json, const char* key, bool* found = nullptr) {
  int position = keyValueStart(json, key);
  if (position < 0) {
    if (found != nullptr) *found = false;
    return "";
  }
  String value;
  bool quoted = false;
  const bool valid = readJsonValue(json, position, value, quoted);
  if (found != nullptr) *found = valid;
  return valid ? value : "";
}

int jsonNumber(const String& json, const char* key, int fallback) {
  bool found = false;
  const String value = jsonValue(json, key, &found);
  if (!found) return fallback;
  if (value.startsWith("0x") || value.startsWith("0X")) return strtol(value.substring(2).c_str(), nullptr, 16);
  return value.toInt();
}

bool jsonBool(const String& json, const char* key, bool fallback) {
  bool found = false;
  const String value = jsonValue(json, key, &found);
  if (!found) return fallback;
  return value == "true" || value == "1";
}

String balancedValue(const String& json, const char* key, char opening, char closing) {
  int position = keyValueStart(json, key);
  if (position < 0) return "";
  position = skipWhitespace(json, position);
  if (position >= static_cast<int>(json.length()) || json[position] != opening) return "";
  const int start = position++;
  int depth = 1;
  bool quoted = false;
  bool escaped = false;
  while (position < static_cast<int>(json.length())) {
    const char character = json[position++];
    if (quoted) {
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') quoted = false;
      continue;
    }
    if (character == '"') quoted = true;
    else if (character == opening) ++depth;
    else if (character == closing && --depth == 0) return json.substring(start, position);
  }
  return "";
}

String jsonObject(const String& json, const char* key) { return balancedValue(json, key, '{', '}'); }
String jsonArray(const String& json, const char* key) { return balancedValue(json, key, '[', ']'); }

String extractObject(const String& json, int start, int& end) {
  if (start < 0 || start >= static_cast<int>(json.length()) || json[start] != '{') return "";
  const int objectStart = start++;
  int depth = 1;
  bool quoted = false;
  bool escaped = false;
  while (start < static_cast<int>(json.length())) {
    const char character = json[start++];
    if (quoted) {
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') quoted = false;
      continue;
    }
    if (character == '"') quoted = true;
    else if (character == '{') ++depth;
    else if (character == '}' && --depth == 0) {
      end = start;
      return json.substring(objectStart, start);
    }
  }
  return "";
}

void parseFields(const String& object, FieldValue* fields, uint8_t& count) {
  count = 0;
  int position = 1;
  while (position < static_cast<int>(object.length()) - 1 && count < MAX_FIELDS) {
    position = skipWhitespace(object, position);
    if (position >= static_cast<int>(object.length()) - 1 || object[position] == '}') break;
    if (object[position] != '"') break;
    String key;
    bool quoted = false;
    if (!readJsonValue(object, position, key, quoted)) break;
    position = skipWhitespace(object, position);
    if (position >= static_cast<int>(object.length()) || object[position++] != ':') break;
    String value;
    if (!readJsonValue(object, position, value, quoted)) break;
    fields[count++] = {key, value};
    position = object.indexOf(',', position);
    if (position < 0) break;
    ++position;
  }
}

String fieldValue(const FieldValue* fields, uint8_t count, const char* key, const String& fallback = "") {
  for (uint8_t index = 0; index < count; ++index) if (fields[index].key == key) return fields[index].value;
  return fallback;
}

int fieldNumber(const FieldValue* fields, uint8_t count, const char* key, int fallback = -1) {
  const String value = fieldValue(fields, count, key);
  if (value.length() == 0) return fallback;
  if (value.startsWith("0x") || value.startsWith("0X")) return strtol(value.substring(2).c_str(), nullptr, 16);
  return value.toInt();
}

bool parseModule(const String& object, ModuleRecord& module) {
  bool hasId = false;
  module = ModuleRecord();
  module.instanceId = jsonValue(object, "instanceId", &hasId);
  module.pluginId = jsonValue(object, "pluginId");
  module.friendlyName = jsonValue(object, "friendlyName");
  module.status = jsonValue(object, "status");
  module.updateIntervalMs = max(1, jsonNumber(object, "updateIntervalMs", 1000));
  module.enabled = jsonBool(object, "enabled", true);
  const String configuration = jsonObject(object, "configuration");
  parseFields(jsonObject(configuration, "pins"), module.pins, module.pinCount);
  parseFields(jsonObject(configuration, "parameters"), module.parameters, module.parameterCount);
  if (module.status.length() == 0) module.status = "ready";
  return hasId && module.pluginId.length() > 0 && configuration.length() > 0;
}

bool parseModulesArray(const String& array, ModuleRecord* output, uint8_t& count) {
  count = 0;
  const int first = skipWhitespace(array, 0);
  if (first >= static_cast<int>(array.length()) || array[first] != '[') return false;
  int position = first + 1;
  while (position < static_cast<int>(array.length()) && count < MAX_MODULES) {
    position = skipWhitespace(array, position);
    if (position >= static_cast<int>(array.length())) return false;
    if (array[position] == ']') return true;
    int end = position;
    const String object = extractObject(array, position, end);
    if (object.length() == 0 || !parseModule(object, output[count])) return false;
    ++count;
    position = skipWhitespace(array, end);
    if (position < static_cast<int>(array.length()) && array[position] == ',') ++position;
    else if (position < static_cast<int>(array.length()) && array[position] != ']') return false;
  }
  return count == MAX_MODULES || (position < static_cast<int>(array.length()) && array[position] == ']');
}

String serializeFields(const FieldValue* fields, uint8_t count, bool numbers) {
  String output = "{";
  for (uint8_t index = 0; index < count; ++index) {
    if (index > 0) output += ',';
    output += "\"" + jsonEscape(fields[index].key) + "\":";
    output += numbers ? fields[index].value : "\"" + jsonEscape(fields[index].value) + "\"";
  }
  return output + "}";
}

int i2cAddress(const ModuleRecord& module) { return fieldNumber(module.parameters, module.parameterCount, "address", -1); }
bool isI2C(const ModuleRecord& module) { return module.pluginId == "sensor.mpu6050" || module.pluginId == "sensor.bme280"; }

String serializeModule(const ModuleRecord& module) {
  String output = "{\"instanceId\":\"" + jsonEscape(module.instanceId) +
                  "\",\"pluginId\":\"" + jsonEscape(module.pluginId) +
                  "\",\"friendlyName\":\"" + jsonEscape(module.friendlyName) +
                  "\",\"configuration\":{\"pins\":" + serializeFields(module.pins, module.pinCount, true) +
                  ",\"parameters\":" + serializeFields(module.parameters, module.parameterCount, false);
  if (isI2C(module)) {
    output += ",\"bus\":{\"kind\":\"I2C\",\"pins\":{\"sda\":" + String(fieldNumber(module.pins, module.pinCount, "sda", 0)) +
              ",\"scl\":" + String(fieldNumber(module.pins, module.pinCount, "scl", 0)) +
              "},\"address\":" + String(i2cAddress(module)) + "}";
  }
  output += "},\"updateIntervalMs\":" + String(module.updateIntervalMs) +
            ",\"enabled\":" + (module.enabled ? "true" : "false") +
            ",\"status\":\"" + jsonEscape(module.status) + "\"}";
  return output;
}

String serializeModules(const ModuleRecord* source = modules, uint8_t count = moduleCount) {
  String output = "[";
  for (uint8_t index = 0; index < count; ++index) {
    if (index > 0) output += ',';
    output += serializeModule(source[index]);
  }
  return output + "]";
}

String sha256Hex(const String& value) {
  mbedtls_sha256_context context;
  uint8_t digest[32] = {};
  char hex[65] = {};
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(&context, reinterpret_cast<const uint8_t*>(value.c_str()), value.length());
  mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);
  for (size_t index = 0; index < sizeof(digest); ++index) snprintf(hex + index * 2, 3, "%02x", digest[index]);
  return String(hex);
}

bool ensureI2CBus(int sda, int scl) {
  if (sda < 0 || scl < 0) return false;
  if (activeI2CSda == sda && activeI2CScl == scl) return true;
  // Arduino-ESP32 keeps the I2C driver initialised after the first begin().
  // Calling Wire.begin() again does not reliably remap an already-running
  // driver to a different pair of GPIOs, so a sensor viewed after another
  // sensor can accidentally keep using the previous module's bus pins.
  if (activeI2CSda >= 0 || activeI2CScl >= 0) {
    Wire.end();
    activeI2CSda = -1;
    activeI2CScl = -1;
    bme280CalibrationCache.valid = false;
  }
  if (!Wire.begin(sda, scl)) return false;
  activeI2CSda = sda;
  activeI2CScl = scl;
  Serial.printf("Ludant: I2C bus selected; SDA GPIO %d, SCL GPIO %d\n", sda, scl);
  return true;
}

bool readI2CRegisterBytes(int address, uint8_t reg, uint8_t* output, size_t length) {
  if (output == nullptr || length == 0 || length > 255) return false;
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, static_cast<uint8_t>(length)) != length) return false;
  for (size_t index = 0; index < length; ++index) {
    if (!Wire.available()) return false;
    output[index] = Wire.read();
  }
  return true;
}

int16_t signedBME28012(uint16_t value) {
  return static_cast<int16_t>((value & 0x0800) != 0 ? value | 0xF000 : value);
}

bool readBME280Calibration(int address, ludant::BME280Calibration& calibration) {
  uint8_t temperaturePressure[24] = {};
  uint8_t humidity[7] = {};
  if (!readI2CRegisterBytes(address, 0x88, temperaturePressure, sizeof(temperaturePressure)) ||
      !readI2CRegisterBytes(address, 0xE1, humidity, sizeof(humidity))) {
    return false;
  }
  auto unsigned16 = [](const uint8_t* bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1]) << 8;
  };
  auto signed16 = [&unsigned16](const uint8_t* bytes, size_t offset) {
    return static_cast<int16_t>(unsigned16(bytes, offset));
  };
  calibration.dig_t1 = unsigned16(temperaturePressure, 0);
  calibration.dig_t2 = signed16(temperaturePressure, 2);
  calibration.dig_t3 = signed16(temperaturePressure, 4);
  calibration.dig_p1 = unsigned16(temperaturePressure, 6);
  calibration.dig_p2 = signed16(temperaturePressure, 8);
  calibration.dig_p3 = signed16(temperaturePressure, 10);
  calibration.dig_p4 = signed16(temperaturePressure, 12);
  calibration.dig_p5 = signed16(temperaturePressure, 14);
  calibration.dig_p6 = signed16(temperaturePressure, 16);
  calibration.dig_p7 = signed16(temperaturePressure, 18);
  calibration.dig_p8 = signed16(temperaturePressure, 20);
  calibration.dig_p9 = signed16(temperaturePressure, 22);
  calibration.dig_h1 = 0;
  if (!readI2CRegisterBytes(address, 0xA1, &calibration.dig_h1, 1)) return false;
  calibration.dig_h2 = signed16(humidity, 0);
  calibration.dig_h3 = humidity[2];
  calibration.dig_h4 = signedBME28012(static_cast<uint16_t>(humidity[3]) << 4 | (humidity[4] & 0x0F));
  calibration.dig_h5 = signedBME28012(static_cast<uint16_t>(humidity[5]) << 4 | (humidity[4] >> 4));
  calibration.dig_h6 = static_cast<int8_t>(humidity[6]);
  return true;
}

bool isAvailableGpio(int gpio) { return (gpio >= 0 && gpio <= 21) || (gpio >= 26 && gpio <= 48); }
bool isAnalogGpio(int gpio) { return gpio >= 1 && gpio <= 10; }
int pinValue(const ModuleRecord& module, const char* key) { return fieldNumber(module.pins, module.pinCount, key, -1); }

bool sharesPin(const ModuleRecord& first, const ModuleRecord& second) {
  for (uint8_t left = 0; left < first.pinCount; ++left) {
    const int firstPin = first.pins[left].value.toInt();
    for (uint8_t right = 0; right < second.pinCount; ++right) {
      if (firstPin == second.pins[right].value.toInt()) return true;
    }
  }
  return false;
}

bool validateModule(const ModuleRecord& module, String& error) {
  if (module.instanceId.length() == 0 || module.pluginId.length() == 0) { error = "pluginId and instanceId are required"; return false; }
  if (module.updateIntervalMs == 0) { error = "updateIntervalMs must be positive"; return false; }
  if (module.pluginId != "sensor.mpu6050" && module.pluginId != "sensor.bme280" &&
      module.pluginId != "sensor.soil-moisture" && module.pluginId != "actuator.relay") {
    error = "this controller does not have that module driver";
    return false;
  }
  for (uint8_t index = 0; index < module.pinCount; ++index) {
    const int gpio = module.pins[index].value.toInt();
    if (!isAvailableGpio(gpio)) { error = "one or more GPIOs are not available"; return false; }
    for (uint8_t previous = 0; previous < index; ++previous) {
      if (gpio == module.pins[previous].value.toInt()) { error = "a GPIO cannot be used twice by one module"; return false; }
    }
  }
  if (isI2C(module)) {
    const int address = i2cAddress(module);
    if (pinValue(module, "sda") < 0 || pinValue(module, "scl") < 0) { error = "SDA and SCL are required"; return false; }
    if (address < 0x03 || address > 0x77) { error = "I2C address is outside the supported range"; return false; }
  } else if (module.pluginId == "sensor.soil-moisture" && !isAnalogGpio(pinValue(module, "signal"))) {
    error = "the selected GPIO does not support analog input";
    return false;
  } else if (module.pluginId == "actuator.relay" && pinValue(module, "output") < 0) {
    error = "relay output GPIO is required";
    return false;
  }
  return true;
}

bool validateModuleSet(const ModuleRecord* candidate, uint8_t count, String& error) {
  for (uint8_t index = 0; index < count; ++index) {
    if (!validateModule(candidate[index], error)) return false;
    for (uint8_t previous = 0; previous < index; ++previous) {
      if (candidate[index].instanceId == candidate[previous].instanceId) { error = "module instance IDs must be unique"; return false; }
      const bool sameI2CBus = isI2C(candidate[index]) && isI2C(candidate[previous]) &&
        pinValue(candidate[index], "sda") == pinValue(candidate[previous], "sda") &&
        pinValue(candidate[index], "scl") == pinValue(candidate[previous], "scl");
      const bool sameAddress = sameI2CBus && i2cAddress(candidate[index]) == i2cAddress(candidate[previous]);
      if (sameAddress) { error = "another I2C module already uses this address"; return false; }
      if (!sameI2CBus && sharesPin(candidate[index], candidate[previous])) { error = "one or more GPIOs are already in use"; return false; }
    }
  }
  return true;
}

bool persistModuleList(const ModuleRecord* candidate, uint8_t count, uint64_t revision = configRevision) {
  const String encoded = serializeModules(candidate, count);
  const String hash = sha256Hex(encoded);
  const uint8_t nextSlot = configSlot == 0 ? 1 : 0;
  const char* modulesKey = nextSlot == 0 ? "modules_a" : "modules_b";
  const char* hashKey = nextSlot == 0 ? "hash_a" : "hash_b";
  const char* revisionKey = nextSlot == 0 ? "rev_a" : "rev_b";
  if (preferences.putString(modulesKey, encoded) == 0) return false;
  if (preferences.putString(hashKey, hash) == 0) return false;
  if (preferences.putULong64(revisionKey, revision) == 0) return false;
  // The active marker is written last. A power loss before this write leaves
  // the previous complete slot active and recoverable.
  if (preferences.putUChar("active_slot", nextSlot) == 0) return false;
  configSlot = nextSlot;
  configRevision = revision;
  configHash = hash;
  return true;
}

void applyRelay(const ModuleRecord& module) {
  if (module.pluginId != "actuator.relay") return;
  const int output = pinValue(module, "output");
  if (!isAvailableGpio(output)) return;
  pinMode(output, OUTPUT);
  const bool state = fieldValue(module.parameters, module.parameterCount, "state", "false") == "true";
  const bool activeHigh = fieldValue(module.parameters, module.parameterCount, "activeHigh", "true") == "true";
  digitalWrite(output, (state == activeHigh) ? HIGH : LOW);
}

bool replaceModuleList(const ModuleRecord* candidate, uint8_t count) {
  const String desired = serializeModules(candidate, count);
  const uint64_t nextRevision = desired == serializeModules() ? configRevision : configRevision + 1;
  if (!persistModuleList(candidate, count, nextRevision)) return false;
  moduleCount = count;
  for (uint8_t index = 0; index < count; ++index) {
    modules[index] = candidate[index];
    applyRelay(modules[index]);
  }
  return true;
}

String capabilitiesJson() {
  return String("[{\"driverId\":\"mpu6050\",\"version\":\"1.0.0\"},")+
         "{\"driverId\":\"bme280\",\"version\":\"1.0.0\"},"+
         "{\"driverId\":\"soil-moisture\",\"version\":\"1.0.0\"},"+
         "{\"driverId\":\"relay\",\"version\":\"1.0.0\"}]";
}

String availableGpiosJson() {
  return "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48]";
}

String i2cScanJson(int sda, int scl) {
  String result = String("{\"sda\":") + String(sda) + ",\"scl\":" + String(scl) + ",\"addresses\":[";
  if (ensureI2CBus(sda, scl)) {
    bool found = false;
    for (int address = 0x03; address <= 0x77; ++address) {
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) {
        if (found) result += ',';
        result += String(address);
        found = true;
      }
    }
  }
  return result + "]}";
}

String stateJson() {
  return String("{\"deviceId\":\"") + jsonEscape(deviceId) +
         "\",\"friendlyName\":\"Ludant controller\",\"firmwareVersion\":\"" + FIRMWARE_VERSION +
         "\",\"protocolVersion\":" + String(PROTOCOL_VERSION) +
         ",\"configRevision\":" + String(static_cast<unsigned long long>(configRevision)) +
         ",\"configHash\":\"" + configHash + "\"" +
         ",\"capabilities\":" + capabilitiesJson() +
         ",\"modules\":" + serializeModules() +
         ",\"availableGPIOs\":" + availableGpiosJson() + "}";
}

uint32_t otaMaxImageSize() {
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  return partition == nullptr ? 0 : partition->size;
}

String deviceInfoJson() {
  // Keep this response below the BLE attribute limit. Detailed diagnostics
  // remain available through the state/diagnostic command path.
  const char* otaAuthorization = "physical_button";
#ifdef LUDANT_OTA_DEVELOPMENT_WINDOW
  otaAuthorization = "physical_button_or_timed_window";
#endif
  return String("{\"deviceId\":\"") + jsonEscape(deviceId) +
         "\",\"device\":\"electronics-controller\",\"chip\":\"ESP32-S3\",\"hardware\":\"" + HARDWARE_TARGET +
         "\",\"firmware\":\"" + FIRMWARE_VERSION +
         "\",\"otaProtocol\":" + String(OTA_PROTOCOL_VERSION) +
         ",\"protocolVersion\":" + String(PROTOCOL_VERSION) +
         ",\"supportsOTA\":true,\"otaCapabilities\":[\"signed\",\"sha256\",\"rollback\",\"sequential_write_with_response\",\"sequential_write_without_response\"],\"supportsModules\":true,\"otaAuthorization\":\"" + otaAuthorization + "\",\"bootVersion\":\"1.0.0\",\"otaMaxImageSize\":" + String(otaMaxImageSize()) +
         "}";
}

class DeviceInfoCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    const String info = deviceInfoJson();
    characteristic->setValue(info);
    Serial.printf("Ludant: device-info read; bytes=%u writes=%u queued=%u processed=%u responses=%u notified=%u dropped=%u connected=%d heap=%u\n",
                  info.length(),
                  controlWriteCount, commandQueuedCount, commandProcessedCount,
                  responseProducedCount, statusNotifyCount, statusDroppedCount,
                  clientConnected ? 1 : 0, ESP.getFreeHeap());
  }
};

void notifyStatus(const String& status) {
  lastStatusBytes = status.length();
  if (status.length() >= MAX_STATUS_LENGTH) {
    statusDroppedCount++;
    statusTooLargeCount++;
    Serial.println(String("Ludant: status too large, bytes=") + String(status.length()));
    return;
  }
  OutboundStatus* outbound = static_cast<OutboundStatus*>(malloc(sizeof(OutboundStatus)));
  if (outbound == nullptr) {
    statusDroppedCount++;
    statusAllocationFailedCount++;
    Serial.println("Ludant: status allocation failed");
    return;
  }
  memcpy(outbound->data, status.c_str(), status.length() + 1);
  const bool telemetry = isTelemetryStatus(status);
  QueueHandle_t destinationQueue = telemetry ? telemetryQueue : responseQueue;
  if (destinationQueue == nullptr || xQueueSend(destinationQueue, outbound, 0) != pdTRUE) {
    free(outbound);
    statusDroppedCount++;
    Serial.printf("Ludant: status queue full; type=%s bytes=%u\n", telemetry ? "telemetry" : "response", status.length());
    logQueueState("status dropped");
    return;
  }
  free(outbound);
  statusQueuedCount++;
  if (!telemetry || millis() - lastTelemetryQueueLogAt >= 5000 || lastTelemetryQueueLogAt == 0) {
    if (telemetry) lastTelemetryQueueLogAt = millis();
    Serial.printf("Ludant: status queued; type=%s bytes=%u pending=%u free=%u\n",
                  telemetry ? "telemetry" : "response", status.length(),
                  destinationQueue == nullptr ? 0 : uxQueueMessagesWaiting(destinationQueue),
                  destinationQueue == nullptr ? 0 : uxQueueSpacesAvailable(destinationQueue));
  }
}

void enqueueBinaryTelemetry(const BinaryTelemetryPacket& packet) {
  if (binaryTelemetryQueue == nullptr || !clientConnected || !binaryTelemetryEnabled) return;
  if (xQueueSend(binaryTelemetryQueue, &packet, 0) == pdTRUE) return;

  BinaryTelemetryPacket discarded = {};
  xQueueReceive(binaryTelemetryQueue, &discarded, 0);
  if (xQueueSend(binaryTelemetryQueue, &packet, 0) != pdTRUE) {
    binaryTelemetryDroppedCount++;
    return;
  }
  binaryTelemetryDroppedCount++;
}

void clearBinaryTelemetryQueue() {
  if (binaryTelemetryQueue == nullptr) return;
  BinaryTelemetryPacket discarded = {};
  while (xQueueReceive(binaryTelemetryQueue, &discarded, 0) == pdTRUE) {}
}

void logStatusBlocked(const char* reason, volatile uint32_t& counter) {
  counter++;
  if (lastStatusBlockedLogAt == 0 || millis() - lastStatusBlockedLogAt >= 1000) {
    lastStatusBlockedLogAt = millis();
    Serial.printf("Ludant: status transmission blocked; reason=%s connected=%d statusReady=%d responseDepth=%u telemetryDepth=%u\n",
                  reason, clientConnected ? 1 : 0, statusCharacteristic != nullptr ? 1 : 0,
                  responseQueue == nullptr ? 0 : uxQueueMessagesWaiting(responseQueue),
                  telemetryQueue == nullptr ? 0 : uxQueueMessagesWaiting(telemetryQueue));
  }
}

void processStatusQueue() {
  if (responseQueue == nullptr || telemetryQueue == nullptr) {
    logStatusBlocked("queue-not-initialized", statusBlockedNoQueueCount);
    activeStatus.active = false;
    return;
  }
  if (statusCharacteristic == nullptr) {
    logStatusBlocked("characteristic-not-ready", statusBlockedNoCharacteristicCount);
    activeStatus.active = false;
    return;
  }
  if (!clientConnected) {
    if ((uxQueueMessagesWaiting(responseQueue) > 0 || uxQueueMessagesWaiting(telemetryQueue) > 0) &&
        (lastDisconnectedQueueLogAt == 0 || millis() - lastDisconnectedQueueLogAt >= 5000)) {
      lastDisconnectedQueueLogAt = millis();
      logQueueState("status waiting without BLE client");
    }
    logStatusBlocked("no-client", statusBlockedNoClientCount);
    activeStatus.active = false;
    return;
  }
  for (uint8_t frame = 0; frame < STATUS_FRAMES_PER_LOOP; ++frame) {
    if (!activeStatus.active) {
      OutboundStatus outbound = {};
      // Responses always win over telemetry so command completion cannot be
      // delayed by a live data stream.
      if (xQueueReceive(responseQueue, &outbound, 0) != pdTRUE &&
          xQueueReceive(telemetryQueue, &outbound, 0) != pdTRUE) return;
      memcpy(activeStatus.data, outbound.data, sizeof(activeStatus.data));
      activeStatus.length = strlen(activeStatus.data);
      activeStatus.nextChunk = 0;
      activeStatus.messageId = statusMessageId++;
      activeStatus.checksum = crc16(activeStatus.data, activeStatus.length);
      // A v2 frame is exactly 16 bytes of compact metadata plus up to four
      // payload bytes, fitting the 20-byte ATT payload of an MTU-23 link.
      activeStatus.totalChunks = max(1, (activeStatus.length + 3) / 4);
      activeStatus.active = true;
      statusTransmissionStartedCount++;
      const bool telemetry = isTelemetryStatus(String(activeStatus.data));
      if (!telemetry || millis() - lastTelemetryTransmissionLogAt >= 5000 || lastTelemetryTransmissionLogAt == 0) {
        if (telemetry) lastTelemetryTransmissionLogAt = millis();
        char checksumHex[5] = {};
        snprintf(checksumHex, sizeof(checksumHex), "%04X", activeStatus.checksum);
        Serial.printf("Ludant: status transmission started; type=%s id=%u bytes=%u chunks=%u crc=%s pending=%u\n",
                      telemetry ? "telemetry" : "response", activeStatus.messageId,
                      activeStatus.length, activeStatus.totalChunks, checksumHex,
                      (responseQueue == nullptr ? 0 : uxQueueMessagesWaiting(responseQueue)) +
                      (telemetryQueue == nullptr ? 0 : uxQueueMessagesWaiting(telemetryQueue)));
      }
    }

    // F2 + id(2) + index(2) + count(3) + length(3) + crc16(4) + data(4).
    const uint16_t index = activeStatus.nextChunk;
    const size_t start = static_cast<size_t>(index) * 4;
    const size_t end = min(start + 4, static_cast<size_t>(activeStatus.length));
    char checksumHex[5] = {};
    snprintf(checksumHex, sizeof(checksumHex), "%04X", activeStatus.checksum);
    String chunk = String("F2") + base36(activeStatus.messageId, 2) + base36(index, 2) +
                   base36(activeStatus.totalChunks, 3) + base36(activeStatus.length, 3) +
                   String(checksumHex) + String(activeStatus.data).substring(start, end);
    statusCharacteristic->setValue(chunk.c_str());
    const bool telemetry = isTelemetryStatus(String(activeStatus.data));
    if (!telemetry && (index == 0 || index + 1 == activeStatus.totalChunks)) {
      logBlePayload("status notify frame", reinterpret_cast<const uint8_t*>(chunk.c_str()), chunk.length());
    }
    statusCharacteristic->notify();
    statusNotifyCount++;
    lastStatusAt = millis();
    activeStatus.nextChunk++;
    if (activeStatus.nextChunk >= activeStatus.totalChunks) {
      activeStatus.active = false;
      if (!telemetry) Serial.printf("Ludant: status transmission complete; id=%u chunks=%u\n", activeStatus.messageId, activeStatus.totalChunks);
    }
  }
}

void processBinaryTelemetryQueue() {
  if (binaryTelemetryQueue == nullptr || telemetryCharacteristic == nullptr ||
      !clientConnected || !binaryTelemetryEnabled) return;

  BinaryTelemetryPacket packet = {};
  if (xQueueReceive(binaryTelemetryQueue, &packet, 0) != pdTRUE) return;

  uint8_t payload[BINARY_TELEMETRY_MAX_PAYLOAD] = {};
  const size_t payloadLength = ludant::encodeBinaryTelemetryPayload(packet, payload, sizeof(payload));
  if (payloadLength == 0) return;

  size_t offset = 0;
  bool first = true;
  while (offset < payloadLength) {
    uint8_t frame[ludant::kBinaryTelemetryFrameLength] = {};
    const size_t headerLength = first ? 11 : 7;
    const size_t capacity = ludant::kBinaryTelemetryFrameLength - headerLength;
    const size_t chunkLength = min(capacity, payloadLength - offset);
    frame[0] = ludant::kBinaryTelemetryFrameMagic;
    frame[1] = ludant::kBinaryTelemetryVersion;
    frame[2] = (first ? ludant::kBinaryTelemetryFrameStart : 0) |
               (offset + chunkLength == payloadLength ? ludant::kBinaryTelemetryFrameEnd : 0);
    frame[3] = static_cast<uint8_t>(packet.sequence & 0xFF);
    frame[4] = static_cast<uint8_t>(packet.sequence >> 8);
    if (first) {
      frame[5] = static_cast<uint8_t>(packet.uptime_ms & 0xFF);
      frame[6] = static_cast<uint8_t>((packet.uptime_ms >> 8) & 0xFF);
      frame[7] = static_cast<uint8_t>((packet.uptime_ms >> 16) & 0xFF);
      frame[8] = static_cast<uint8_t>((packet.uptime_ms >> 24) & 0xFF);
      frame[9] = static_cast<uint8_t>(payloadLength & 0xFF);
      frame[10] = static_cast<uint8_t>(payloadLength >> 8);
    } else {
      frame[5] = static_cast<uint8_t>(offset & 0xFF);
      frame[6] = static_cast<uint8_t>(offset >> 8);
    }
    memcpy(frame + headerLength, payload + offset, chunkLength);
    telemetryCharacteristic->setValue(frame, headerLength + chunkLength);
    telemetryCharacteristic->notify();
    offset += chunkLength;
    first = false;
  }
}

void sendResponse(const String& requestId, bool ok, const String& payload = "", const String& error = "") {
  String response = String("{\"type\":\"response\",\"requestId\":\"") + jsonEscape(requestId) +
                    "\",\"ok\":" + (ok ? "true" : "false") +
                    ",\"protocolVersion\":" + String(PROTOCOL_VERSION) +
                    ",\"configRevision\":" + String(static_cast<unsigned long long>(configRevision)) +
                    ",\"configHash\":\"" + configHash + "\"";
  if (ok && payload.length() > 0) response += ",\"payload\":" + payload;
  if (!ok && error.length() > 0) response += ",\"error\":\"" + jsonEscape(error) + "\"";
  response += "}";
  lastResponseBytes = response.length();
  responseProducedCount++;
  lastResponseAt = millis();
  Serial.printf("Ludant: response produced; requestId=%s ok=%d bytes=%u revision=%llu error=%s\n",
                requestId.c_str(), ok ? 1 : 0, response.length(),
                static_cast<unsigned long long>(configRevision),
                error.length() > 0 ? error.c_str() : "none");
  notifyStatus(response);
}

bool loadModules() {
  const uint8_t activeSlot = preferences.getUChar("active_slot", 255);
  String stored;
  uint8_t loadedSlot = 255;
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    const uint8_t slot = attempt == 0 ? activeSlot : (activeSlot == 0 ? 1 : 0);
    if (slot > 1) continue;
    const char* modulesKey = slot == 0 ? "modules_a" : "modules_b";
    const char* hashKey = slot == 0 ? "hash_a" : "hash_b";
    const String encoded = preferences.getString(modulesKey, "");
    const String storedHash = preferences.getString(hashKey, "");
    if (encoded.length() > 0 && storedHash == sha256Hex(encoded)) {
      stored = encoded;
      loadedSlot = slot;
      break;
    }
  }
  // Migrate configurations written by pre-revision Arduino images. They are
  // valid data, but begin at revision zero until the next publish.
  if (stored.length() == 0) {
    stored = preferences.getString("modules", "[]");
    loadedSlot = 255;
  }
  ModuleRecord* loaded = candidateModules;
  uint8_t loadedCount = 0;
  if (!parseModulesArray(stored, loaded, loadedCount)) return false;
  String error;
  if (!validateModuleSet(loaded, loadedCount, error)) return false;
  configSlot = loadedSlot <= 1 ? loadedSlot : 0;
  if (loadedSlot <= 1) {
    const char* revisionKey = loadedSlot == 0 ? "rev_a" : "rev_b";
    configRevision = preferences.getULong64(revisionKey, 0);
    const char* hashKey = loadedSlot == 0 ? "hash_a" : "hash_b";
    configHash = preferences.getString(hashKey, sha256Hex(stored));
  } else {
    configRevision = 0;
    configHash = sha256Hex(stored);
  }
  moduleCount = loadedCount;
  for (uint8_t index = 0; index < moduleCount; ++index) {
    modules[index] = loaded[index];
    applyRelay(modules[index]);
  }
  return true;
}

bool candidateFromJson(const String& json, ModuleRecord* candidate, uint8_t& count) {
  const String array = jsonArray(json, "modules");
  return array.length() > 0 && parseModulesArray(array, candidate, count);
}

void emitTelemetry() {
  int index = -1;
  for (uint8_t current = 0; current < moduleCount; ++current) {
    if (modules[current].instanceId == telemetryInstance) { index = current; break; }
  }
  if (index < 0 || !modules[index].enabled) {
    if (millis() - telemetryLastErrorLogAt >= 5000 || telemetryLastErrorLogAt == 0) {
      telemetryLastErrorLogAt = millis();
      Serial.printf("Ludant: telemetry skipped; instanceId=%s found=%d enabled=%d configuredModules=%u\n",
                    telemetryInstance.c_str(), index >= 0 ? 1 : 0,
                    index >= 0 && modules[index].enabled ? 1 : 0, moduleCount);
    }
    return;
  }
  const ModuleRecord& module = modules[index];
  const bool useBinary = binaryTelemetryEnabled;
  String values = useBinary ? String() : String("{");
  String telemetryError;
  bool hasValue = false;
  BinaryTelemetryPacket binaryPacket = {};
  binaryPacket.uptime_ms = millis();
  auto addValue = [&](const char* key, double value) {
    if (useBinary) {
      if (binaryPacket.field_count >= ludant::kBinaryTelemetryMaxFields) return;
      auto& field = binaryPacket.fields[binaryPacket.field_count++];
      strncpy(field.name, key, ludant::kBinaryTelemetryFieldNameLength);
      field.name[ludant::kBinaryTelemetryFieldNameLength] = '\0';
      field.value = static_cast<float>(value);
      hasValue = true;
      return;
    }
    if (hasValue) values += ',';
    values += "\"" + String(key) + "\":" + String(value, 4);
    hasValue = true;
  };
  if (module.pluginId == "actuator.relay") {
    addValue("state", fieldValue(module.parameters, module.parameterCount, "state", "false") == "true" ? 1 : 0);
  } else if (module.pluginId == "sensor.soil-moisture") {
    const int raw = analogRead(pinValue(module, "signal"));
    const int dry = fieldNumber(module.parameters, module.parameterCount, "dryValue", 3200);
    const int wet = fieldNumber(module.parameters, module.parameterCount, "wetValue", 1400);
    const double moisture = dry == wet ? 0.0 : constrain(100.0 * (dry - raw) / static_cast<double>(dry - wet), 0.0, 100.0);
    addValue("moisture", moisture);
  } else if (isI2C(module)) {
    const int sda = pinValue(module, "sda");
    const int scl = pinValue(module, "scl");
    const int address = i2cAddress(module);
    if (!ensureI2CBus(sda, scl)) {
      telemetryError = String("I2C bus is invalid (SDA GPIO ") + String(sda) + ", SCL GPIO " + String(scl) + ")";
    }
    if (module.pluginId == "sensor.mpu6050") {
      uint8_t whoAmI = 0;
      bool identified = false;
      if (telemetryError.length() == 0) {
        Wire.beginTransmission(address); Wire.write(0x75);
        const bool registerSelected = Wire.endTransmission(false) == 0;
        const uint8_t receivedWhoAmI = registerSelected ? Wire.requestFrom(address, 1) : 0;
        if (receivedWhoAmI == 1) {
          whoAmI = Wire.read();
          identified = whoAmI == 0x68 || whoAmI == 0x69 || whoAmI == 0x70;
        }
      }
      // MPU6050 powers up asleep. Wake it before attempting the burst read.
      Wire.beginTransmission(address); Wire.write(0x6B); Wire.write(0x00);
      const bool woke = telemetryError.length() == 0 && Wire.endTransmission() == 0;
      uint8_t bytes[14] = {};
      Wire.beginTransmission(address); Wire.write(0x3B);
      const bool readRequestSent = Wire.endTransmission(false) == 0;
      const bool sent = woke && identified && readRequestSent;
      const uint8_t received = sent ? Wire.requestFrom(address, 14) : 0;
      if (sent && received == 14) {
        for (uint8_t byteIndex = 0; byteIndex < 14; ++byteIndex) bytes[byteIndex] = Wire.read();
        auto signedValue = [&bytes](uint8_t offset) { return static_cast<int16_t>((static_cast<uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]); };
        const double accelX = signedValue(0) / 16384.0;
        const double accelY = signedValue(2) / 16384.0;
        const double accelZ = signedValue(4) / 16384.0;
        const double temperature = signedValue(6) / 340.0 + 36.53;
        const double gyroX = signedValue(8) / 131.0;
        const double gyroY = signedValue(10) / 131.0;
        const double gyroZ = signedValue(12) / 131.0;
        addValue("accelX", accelX); addValue("accelY", accelY); addValue("accelZ", accelZ);
        addValue("temperature", temperature); addValue("gyroX", gyroX); addValue("gyroY", gyroY); addValue("gyroZ", gyroZ);
      } else {
        telemetryError = String("MPU6050 WHO_AM_I/read failed (0x") + String(whoAmI, HEX) + ") at I2C address 0x" + String(address, HEX) +
                         " (SDA GPIO " + String(sda) + ", SCL GPIO " + String(scl) + ")";
      }
    } else {
      uint8_t bytes[8] = {};
      const bool cacheMatches = bme280CalibrationCache.valid &&
                                bme280CalibrationCache.sda == sda &&
                                bme280CalibrationCache.scl == scl &&
                                bme280CalibrationCache.address == address;
      if (!cacheMatches) {
        ludant::BME280Calibration calibration;
        bme280CalibrationCache.valid = readBME280Calibration(address, calibration);
        if (bme280CalibrationCache.valid) {
          bme280CalibrationCache.sda = sda;
          bme280CalibrationCache.scl = scl;
          bme280CalibrationCache.address = address;
          bme280CalibrationCache.value = calibration;
          Serial.printf("Ludant: BME280 calibration cached; address=0x%02X, SDA GPIO %d, SCL GPIO %d\n", address, sda, scl);
        }
      }
      if (bme280CalibrationCache.valid &&
          readI2CRegisterBytes(address, 0xF7, bytes, sizeof(bytes))) {
        const int pressure = (static_cast<int>(bytes[0]) << 12) | (static_cast<int>(bytes[1]) << 4) | (bytes[2] >> 4);
        const int temperature = (static_cast<int>(bytes[3]) << 12) | (static_cast<int>(bytes[4]) << 4) | (bytes[5] >> 4);
        const int humidity = (static_cast<int>(bytes[6]) << 8) | bytes[7];
        double temperatureC = 0.0;
        double humidityPercent = 0.0;
        double pressureHpa = 0.0;
        if (ludant::compensateBME280(bme280CalibrationCache.value, pressure, temperature, humidity,
                                     temperatureC, humidityPercent, pressureHpa)) {
          addValue("pressure", pressureHpa);
          addValue("temperature", temperatureC);
          addValue("humidity", humidityPercent);
        } else {
          telemetryError = "BME280 calibration data is invalid";
        }
      }
      if (!hasValue && telemetryError.length() == 0) {
        telemetryError = String("I2C sensor not responding at address 0x") + String(address, HEX) +
                         " (SDA GPIO " + String(sda) + ", SCL GPIO " + String(scl) + ")";
      }
    }
  }

  if (useBinary) {
    binaryPacket.sequence = static_cast<uint16_t>(telemetrySequence++);
    binaryPacket.quality = binaryPacket.field_count > 0 && telemetryError.length() == 0 ? 0 : 1;
    if (telemetryError.length() > 0) {
      strncpy(binaryPacket.error, telemetryError.c_str(), ludant::kBinaryTelemetryErrorLength);
      binaryPacket.error[ludant::kBinaryTelemetryErrorLength] = '\0';
    } else if (binaryPacket.quality != 0) {
      strncpy(binaryPacket.error, "No telemetry values available", ludant::kBinaryTelemetryErrorLength);
    }
    enqueueBinaryTelemetry(binaryPacket);
    return;
  }
  if (values.length() == 0) values = "{";
  values += "}";
  String packet = String("{\"type\":\"telemetry\",\"protocolVersion\":") + String(PROTOCOL_VERSION) +
               ",\"sequence\":" + String(telemetrySequence++) +
               ",\"uptimeMs\":" + String(millis()) +
               ",\"deviceId\":\"" + jsonEscape(deviceId) +
               "\",\"instanceId\":\"" + jsonEscape(telemetryInstance) +
               "\",\"quality\":\"" + (telemetryError.length() == 0 ? "ok" : "unavailable") + "\"";
  if (telemetryError.length() > 0) {
    packet += ",\"error\":\"" + jsonEscape(telemetryError) + "\"";
    if (millis() - telemetryLastErrorLogAt >= 5000 || telemetryLastErrorLogAt == 0) {
      telemetryLastErrorLogAt = millis();
      Serial.println(String("Ludant: telemetry error: ") + telemetryError);
    }
  }
  packet += ",\"values\":" + values + "}";
  notifyStatus(packet);
}

void handleModuleCommand(const String& message, const String& command, const String& requestId) {
  if (command == "get_state") {
    Serial.printf("Ludant: get_state snapshot; modules=%u revision=%llu hash=%s\n",
                  moduleCount, static_cast<unsigned long long>(configRevision), configHash.c_str());
    sendResponse(requestId, true, String("{\"state\":") + stateJson() + "}");
    return;
  }
  if (command == "scan_i2c") {
    const int sda = jsonNumber(message, "sda", -1);
    const int scl = jsonNumber(message, "scl", -1);
    if (!isAvailableGpio(sda) || !isAvailableGpio(scl)) {
      sendResponse(requestId, false, "", "SDA and SCL must be valid GPIOs");
    } else {
      sendResponse(requestId, true, i2cScanJson(sda, scl));
    }
    return;
  }
  if (command == "apply_modules") {
    ModuleRecord* candidate = candidateModules;
    uint8_t count = 0;
    String error;
    Serial.printf("Ludant: apply_modules requested; currentRevision=%llu currentModules=%u\n",
                  static_cast<unsigned long long>(configRevision), moduleCount);
    if (!candidateFromJson(message, candidate, count)) {
      Serial.println("Ludant: apply_modules rejected; modules array could not be parsed");
      sendResponse(requestId, false, "", "modules must be an array"); return;
    }
    Serial.printf("Ludant: apply_modules candidate parsed; modules=%u\n", count);
    if (!validateModuleSet(candidate, count, error)) {
      Serial.println(String("Ludant: apply_modules rejected validation: ") + error);
      sendResponse(requestId, false, "", error); return;
    }
    bool hasBaseRevision = false;
    const String requestedRevision = jsonValue(message, "baseRevision", &hasBaseRevision);
    Serial.printf("Ludant: apply_modules revision check; supplied=%d baseRevision=%s currentRevision=%llu\n",
                  hasBaseRevision ? 1 : 0, hasBaseRevision ? requestedRevision.c_str() : "none",
                  static_cast<unsigned long long>(configRevision));
    if (hasBaseRevision && strtoull(requestedRevision.c_str(), nullptr, 10) != configRevision) {
      Serial.println("Ludant: apply_modules rejected; stale base revision");
      sendResponse(requestId, false, "", "configuration revision is stale; reload controller state");
      return;
    }
    if (!replaceModuleList(candidate, count)) {
      Serial.println("Ludant: apply_modules rejected; persistence failed");
      sendResponse(requestId, false, "", "could not persist the module configuration"); return;
    }
    Serial.printf("Ludant: apply_modules committed; modules=%u revision=%llu hash=%s\n",
                  moduleCount, static_cast<unsigned long long>(configRevision), configHash.c_str());
    sendResponse(requestId, true, String("{\"state\":") + stateJson() + "}");
    return;
  }
  if (command == "configure_module") {
    const String object = jsonObject(message, "module");
    ModuleRecord& configured = configuredModule;
    ModuleRecord* candidate = candidateModules;
    uint8_t count = 0;
    String error;
    if (object.length() == 0 || !parseModule(object, configured)) { sendResponse(requestId, false, "", "module is required"); return; }
    for (uint8_t index = 0; index < moduleCount; ++index) if (modules[index].instanceId != configured.instanceId) candidate[count++] = modules[index];
    if (count < MAX_MODULES) candidate[count++] = configured;
    if (!validateModuleSet(candidate, count, error)) { sendResponse(requestId, false, "", error); return; }
    if (!replaceModuleList(candidate, count)) { sendResponse(requestId, false, "", "could not persist the module configuration"); return; }
    sendResponse(requestId, true, String("{\"state\":") + stateJson() + "}");
    return;
  }
  if (command == "remove_module") {
    const String requestedId = jsonValue(message, "instanceId");
    ModuleRecord* candidate = candidateModules;
    uint8_t count = 0;
    for (uint8_t index = 0; index < moduleCount; ++index) if (modules[index].instanceId != requestedId) candidate[count++] = modules[index];
    if (!replaceModuleList(candidate, count)) { sendResponse(requestId, false, "", "could not persist the module configuration"); return; }
    sendResponse(requestId, true, String("{\"state\":") + stateJson() + "}");
    return;
  }
  if (command == "set_module_enabled") {
    const String requestedId = jsonValue(message, "instanceId");
    for (uint8_t index = 0; index < moduleCount; ++index) candidateModules[index] = modules[index];
    for (uint8_t index = 0; index < moduleCount; ++index) if (candidateModules[index].instanceId == requestedId) candidateModules[index].enabled = jsonBool(message, "enabled", candidateModules[index].enabled);
    if (!replaceModuleList(candidateModules, moduleCount)) { sendResponse(requestId, false, "", "could not persist the module configuration"); return; }
    sendResponse(requestId, true, String("{\"state\":") + stateJson() + "}");
    return;
  }
  if (command == "start_telemetry" || command == "set_live_interval") {
    telemetryInstance = jsonValue(message, "instanceId");
    telemetryIntervalMs = max(50, jsonNumber(message, "intervalMs", 1000));
    binaryTelemetryEnabled = jsonValue(message, "telemetryTransport") == "binary-v2" && telemetryCharacteristic != nullptr;
    clearBinaryTelemetryQueue();
    Serial.printf("Ludant: telemetry configured; command=%s instanceId=%s intervalMs=%u\n",
                  command.c_str(), telemetryInstance.c_str(), telemetryIntervalMs);
    Serial.printf("Ludant: telemetry transport=%s\n", binaryTelemetryEnabled ? "binary-v2" : "json-legacy");
    telemetryRunning = true;
    telemetryNextAt = 0;
    sendResponse(requestId, true);
    return;
  }
  if (command == "stop_telemetry") {
    telemetryRunning = false;
    telemetryInstance = "";
    binaryTelemetryEnabled = false;
    clearBinaryTelemetryQueue();
    sendResponse(requestId, true);
    return;
  }
  sendResponse(requestId, false, "", "unknown controller command");
}

void processControlMessage(const String& message) {
  const String command = jsonValue(message, "command");
  const String requestId = jsonValue(message, "requestId");
  logBlePayload("control message", reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
  Serial.println(String("Ludant: processing command=") + command + ", requestId=" + requestId + ", bytes=" + String(message.length()) + ", configRevision=" + String(static_cast<unsigned long long>(configRevision)));
  commandProcessedCount++;
  lastCommandAt = millis();
  if (command == "ota_authorization_status") {
      if (otaActive) {
        notifyStatus("ERROR:OTA_BUSY:controller commands are disabled during firmware update");
      } else {
        notifyStatus(otaPermissionAllowed() ? "AUTHORIZATION_READY" : "WAITING_FOR_BOOT");
      }
  } else if (command == "begin") {
      expectedSize = jsonNumber(message, "size", 0);
      expectedHash = jsonValue(message, "sha256");
      expectedVersion = jsonValue(message, "version");
      expectedSignature = jsonObject(message, "signature");
      bool hasAlgorithm = false;
      const String signatureAlgorithm = jsonValue(expectedSignature, "algorithm", &hasAlgorithm);
      const String signatureValue = jsonValue(expectedSignature, "value");
      const String product = jsonValue(message, "product");
      const String hardware = jsonValue(message, "hardware");
      const uint32_t otaProtocol = jsonNumber(message, "otaProtocol", 0);
      uint8_t expectedDigest[32] = {};
      const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
      if (otaActive) {
        notifyStatus("ERROR:ALREADY_ACTIVE:an OTA update is already in progress");
        return;
      }
      if (!otaPermissionAllowed()) {
        notifyStatus("ERROR:WINDOW_CLOSED:hold BOOT while starting the firmware update");
        return;
      }
      if (expectedSize == 0 || expectedHash.length() != 64 || !decodeExpectedHash(expectedHash, expectedDigest) ||
          destination == nullptr || expectedSize > destination->size) {
        notifyStatus("ERROR:INVALID_BEGIN:invalid image metadata or image is too large");
        return;
      }
      if (product != "ludant-esp32s3" || hardware != HARDWARE_TARGET || otaProtocol != OTA_PROTOCOL_VERSION) {
        notifyStatus("ERROR:INCOMPATIBLE_ARTIFACT:product, hardware, or OTA protocol is not supported");
        return;
      }
      if (!validFirmwareVersion(expectedVersion)) {
        notifyStatus("ERROR:INVALID_VERSION:version must end in major.minor.patch");
        return;
      }
      if (!hasAlgorithm || signatureAlgorithm != "ed25519" || signatureValue.isEmpty() ||
          !ludant::ota_auth::verifyDigestSignature(expectedDigest, signatureValue.c_str())) {
        notifyStatus("ERROR:SIGNATURE_INVALID:firmware signature verification failed");
        return;
      }
      const String rollbackFloor = preferences.getString("ota_floor", FIRMWARE_VERSION);
      if (compareFirmwareVersions(expectedVersion, rollbackFloor) <= 0) {
        notifyStatus("ERROR:VERSION_REJECTED:firmware version is not newer than the installed version");
        return;
      }
      const uint32_t otaPreparationStartedAt = millis();
      notifyStatus("PREPARING");
      otaDestination = destination;
      if (esp_ota_begin(otaDestination, expectedSize, &otaHandle) != ESP_OK) {
        otaDestination = nullptr;
        notifyStatus("ERROR:OTA_BEGIN:could not prepare the inactive OTA partition");
        return;
      }
      Serial.printf("Ludant: OTA preparation complete; durationMs=%u imageBytes=%u\n",
                    millis() - otaPreparationStartedAt, expectedSize);
      otaHandleValid = true;
      receivedSize = 0;
      otaQueuedBytes = 0;
      otaDataQueueFault = false;
      otaDataQueueFaultMessage[0] = '\0';
      otaDataWriteCount = 0;
      otaDataWriteFailures = 0;
      otaLastWriteDurationUs = 0;
      if (otaDataQueue != nullptr) xQueueReset(otaDataQueue);
      mbedtls_sha256_init(&shaContext);
      mbedtls_sha256_starts(&shaContext, 0);
      shaActive = true;
      otaActive = true;
      sendResponse(requestId, true);
      notifyStatus("READY");
  } else if (command == "end") {
      if (!otaActive) {
        notifyStatus("ERROR:NOT_ACTIVE:END received without an active OTA update");
        return;
      }
      if (receivedSize != expectedSize) {
        clearOtaState(true);
        notifyStatus("ERROR:SIZE_MISMATCH:received bytes do not match the declared image size");
        return;
      }
      notifyStatus("VERIFYING");
      uint8_t digest[32] = {};
      char digestHex[65] = {};
      if (shaActive) {
        mbedtls_sha256_finish(&shaContext, digest);
        mbedtls_sha256_free(&shaContext);
        shaActive = false;
        for (size_t index = 0; index < sizeof(digest); ++index) snprintf(digestHex + index * 2, 3, "%02x", digest[index]);
      }
      if (expectedHash != String(digestHex)) {
        clearOtaState(true);
        notifyStatus("ERROR:SHA_MISMATCH:firmware integrity hash does not match");
        return;
      }
      otaHandleValid = false;
      const esp_err_t imageResult = esp_ota_end(otaHandle);
      otaHandle = 0;
      if (imageResult != ESP_OK) {
        clearOtaState(false);
        notifyStatus("ERROR:IMAGE_INVALID:ESP image validation failed");
        return;
      }
      esp_app_desc_t description{};
      if (otaDestination == nullptr || esp_ota_get_partition_description(otaDestination, &description) != ESP_OK ||
          expectedVersion != String(description.version)) {
        clearOtaState(false);
        notifyStatus("ERROR:VERSION_MISMATCH:requested version does not match image metadata");
        return;
      }
      if (esp_ota_set_boot_partition(otaDestination) != ESP_OK) {
        clearOtaState(false);
        notifyStatus("ERROR:SET_BOOT:could not select the verified OTA partition");
        return;
      }
      clearOtaState(false);
      sendResponse(requestId, true);
      notifyStatus("SUCCESS");
      delay(1000);
      ESP.restart();
  } else if (command == "abort") {
      clearOtaState(true);
      notifyStatus("ABORTED");
  } else if (otaActive) {
      notifyStatus("ERROR:OTA_BUSY:controller commands are disabled during firmware update");
  } else {
    handleModuleCommand(message, command, requestId);
  }
}

bool enqueueControlCommand(const uint8_t* data, size_t length) {
  if (length >= MAX_STATUS_LENGTH) {
    Serial.println(String("Ludant: command too large, bytes=") + String(length));
    return false;
  }
  InboundCommand* inbound = static_cast<InboundCommand*>(malloc(sizeof(InboundCommand)));
  if (inbound == nullptr) {
    Serial.println("Ludant: command allocation failed");
    return false;
  }
  memcpy(inbound->data, data, length);
  inbound->data[length] = '\0';
  if (controlQueue == nullptr || xQueueSend(controlQueue, inbound, 0) != pdTRUE) {
    free(inbound);
    Serial.printf("Ludant: command queue full; bytes=%u\n", length);
    logQueueState("command dropped");
    return false;
  }
  free(inbound);
  Serial.printf("Ludant: command queued; bytes=%u pending=%u free=%u\n", length,
                controlQueue == nullptr ? 0 : uxQueueMessagesWaiting(controlQueue),
                controlQueue == nullptr ? 0 : uxQueueSpacesAvailable(controlQueue));
  commandQueuedCount++;
  return true;
}

void resetActiveCommand() {
  activeCommand = ActiveCommand{};
}

void processCommandFrame(const uint8_t* data, size_t length) {
  if (activeCommand.active && millis() - activeCommand.updatedAt >= 5000) {
    Serial.printf("Ludant: command frame assembly expired; id=%u received=%u/%u\n",
                  activeCommand.messageId, activeCommand.nextChunk, activeCommand.totalChunks);
    resetActiveCommand();
  }
  if (length < COMMAND_FRAME_HEADER_LENGTH || data[0] != COMMAND_FRAME_MAGIC || data[1] != COMMAND_FRAME_VERSION) {
    Serial.printf("Ludant: invalid command frame header; bytes=%u magic=0x%02X version=%u\n",
                  length, length > 0 ? data[0] : 0, length > 1 ? data[1] : 0);
    resetActiveCommand();
    return;
  }

  const uint16_t messageId = readLittleEndian16(data + 2);
  const uint16_t index = readLittleEndian16(data + 4);
  const uint16_t totalChunks = readLittleEndian16(data + 6);
  const uint16_t messageLength = readLittleEndian16(data + 8);
  const uint16_t checksum = readLittleEndian16(data + 10);
  const size_t payloadLength = length - COMMAND_FRAME_HEADER_LENGTH;

  if (totalChunks == 0 || index >= totalChunks || messageLength == 0 ||
      messageLength >= MAX_STATUS_LENGTH || payloadLength == 0 ||
      payloadLength > messageLength) {
    Serial.printf("Ludant: invalid command frame metadata; id=%u index=%u/%u messageBytes=%u payloadBytes=%u\n",
                  messageId, index, totalChunks, messageLength, payloadLength);
    resetActiveCommand();
    return;
  }

  if (!activeCommand.active) {
    if (index != 0) {
      Serial.printf("Ludant: command frame started out of order; id=%u index=%u\n", messageId, index);
      return;
    }
    activeCommand.active = true;
    activeCommand.messageId = messageId;
    activeCommand.messageLength = messageLength;
    activeCommand.totalChunks = totalChunks;
    activeCommand.checksum = checksum;
  } else if (activeCommand.messageId != messageId ||
             activeCommand.messageLength != messageLength ||
             activeCommand.totalChunks != totalChunks ||
             activeCommand.checksum != checksum) {
    Serial.printf("Ludant: command frame metadata changed mid-message; activeId=%u receivedId=%u\n",
                  activeCommand.messageId, messageId);
    resetActiveCommand();
    return;
  }

  if (index != activeCommand.nextChunk || activeCommand.length + payloadLength >= MAX_STATUS_LENGTH) {
    Serial.printf("Ludant: command frame out of order or too large; id=%u index=%u expected=%u assembled=%u payload=%u\n",
                  messageId, index, activeCommand.nextChunk, activeCommand.length, payloadLength);
    resetActiveCommand();
    return;
  }

  memcpy(activeCommand.data + activeCommand.length, data + COMMAND_FRAME_HEADER_LENGTH, payloadLength);
  activeCommand.length += payloadLength;
  activeCommand.nextChunk++;
  activeCommand.updatedAt = millis();
  if (index == 0 || index + 1 == totalChunks) {
    Serial.printf("Ludant: command frame received; id=%u frame=%u/%u bytes=%u assembled=%u/%u\n",
                  messageId, index + 1, totalChunks, length, activeCommand.length, messageLength);
  }

  if (activeCommand.nextChunk < activeCommand.totalChunks) return;
  const bool valid = activeCommand.length == activeCommand.messageLength &&
                     crc16Bytes(reinterpret_cast<const uint8_t*>(activeCommand.data), activeCommand.length) == activeCommand.checksum;
  if (!valid) {
    Serial.printf("Ludant: command frame checksum/length failed; id=%u assembled=%u expected=%u crc=%04X expected=%04X\n",
                  messageId, activeCommand.length, activeCommand.messageLength,
                  crc16Bytes(reinterpret_cast<const uint8_t*>(activeCommand.data), activeCommand.length), activeCommand.checksum);
    resetActiveCommand();
    return;
  }

  Serial.printf("Ludant: command frame reassembled; id=%u chunks=%u bytes=%u crc=%04X\n",
                messageId, activeCommand.totalChunks, activeCommand.length, activeCommand.checksum);
  enqueueControlCommand(reinterpret_cast<const uint8_t*>(activeCommand.data), activeCommand.length);
  resetActiveCommand();
}

class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    controlWriteCount++;
    lastControlWriteAt = millis();
    logBlePayload("control write received", characteristic->getData(), length);
    const uint8_t* data = characteristic->getData();
    if (length > 0 && data[0] == COMMAND_FRAME_MAGIC) {
      processCommandFrame(data, length);
      return;
    }
    enqueueControlCommand(data, length);
  }
};

void processControlQueue() {
  if (controlQueue == nullptr) return;
  // Only loopTask consumes this queue. Do not retain a 1 KB local buffer
  // across the parser, validation, and NVS call chain.
  static InboundCommand inbound = {};
  if (xQueueReceive(controlQueue, &inbound, 0) == pdTRUE) {
    const String command = jsonValue(String(inbound.data), "command");
    if (command == "end") drainOtaDataQueue();
    processControlMessage(String(inbound.data));
    Serial.printf("Ludant: command complete, loop stack minimum free=%u bytes\n",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    logQueueState("command complete");
  }
}

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t valueLength = characteristic->getLength();
    if (!otaActive || !otaHandleValid) {
      if (lastIgnoredDataLogAt == 0 || millis() - lastIgnoredDataLogAt >= 5000) {
        lastIgnoredDataLogAt = millis();
        Serial.printf("Ludant: BLE firmware-data write ignored; otaActive=0 bytes=%u\n", valueLength);
      }
      return;
    }
    if (valueLength == 0 || valueLength > OTA_DATA_CHUNK_CAPACITY ||
        receivedSize > expectedSize || otaQueuedBytes > expectedSize - receivedSize ||
        valueLength > expectedSize - receivedSize - otaQueuedBytes) {
      otaDataQueueFault = true;
      strncpy(otaDataQueueFaultMessage, "firmware data exceeded the declared image size", sizeof(otaDataQueueFaultMessage) - 1);
      return;
    }
    if (otaDataQueue == nullptr) {
      otaDataQueueFault = true;
      strncpy(otaDataQueueFaultMessage, "OTA data queue unavailable", sizeof(otaDataQueueFaultMessage) - 1);
      return;
    }
    OtaDataChunk chunk = {};
    chunk.length = valueLength;
    memcpy(chunk.data, characteristic->getData(), valueLength);
    if (xQueueSend(otaDataQueue, &chunk, 0) != pdTRUE) {
      otaDataQueueFault = true;
      strncpy(otaDataQueueFaultMessage, "OTA data queue full", sizeof(otaDataQueueFaultMessage) - 1);
      return;
    }
    otaQueuedBytes += valueLength;
    Serial.printf("Ludant: OTA data queued; chunkBytes=%u received=%u/%u queueBytes=%u queueDepth=%u\n",
                  valueLength, receivedSize, expectedSize, otaQueuedBytes,
                  uxQueueMessagesWaiting(otaDataQueue));
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    clientConnected = true;
    bleConnectCount++;
    lastBleConnectAt = millis();
    // Telemetry is scoped to a BLE session. A mobile app can be terminated
    // without the previous disconnect callback reaching the controller, so
    // start every new client from a clean telemetry/status state.
    activeStatus.active = false;
    resetActiveCommand();
    if (responseQueue != nullptr) xQueueReset(responseQueue);
    if (telemetryQueue != nullptr) xQueueReset(telemetryQueue);
    if (binaryTelemetryQueue != nullptr) xQueueReset(binaryTelemetryQueue);
    telemetryRunning = false;
    binaryTelemetryEnabled = false;
    telemetryInstance = "";
    telemetryNextAt = 0;
    Serial.printf("Ludant: BLE client connected; connects=%u disconnects=%u heap=%u\n",
                  bleConnectCount, bleDisconnectCount, ESP.getFreeHeap());
    Serial.println("Ludant: new BLE session; status queues reset and telemetry stopped");
    logQueueState("client connected");
  }
  void onDisconnect(BLEServer* server) override {
    clientConnected = false;
    bleDisconnectCount++;
    lastBleDisconnectAt = millis();
    Serial.printf("Ludant: BLE client disconnected; connects=%u disconnects=%u heap=%u\n",
                  bleConnectCount, bleDisconnectCount, ESP.getFreeHeap());
    activeStatus.active = false;
    resetActiveCommand();
    if (responseQueue != nullptr) xQueueReset(responseQueue);
    if (telemetryQueue != nullptr) xQueueReset(telemetryQueue);
    if (binaryTelemetryQueue != nullptr) xQueueReset(binaryTelemetryQueue);
    telemetryRunning = false;
    binaryTelemetryEnabled = false;
    if (otaActive) {
      otaDataQueueFault = true;
      strncpy(otaDataQueueFaultMessage, "BLE client disconnected", sizeof(otaDataQueueFaultMessage) - 1);
      Serial.println("Ludant: OTA disconnect abort queued for the main loop");
    }
    Serial.println("Ludant: client disconnected; status queue reset and telemetry stopped");
    logQueueState("client disconnected");
    server->startAdvertising();
  }
};

void confirmRunningApplication() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state{};
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    Serial.printf("Ludant: active partition=%s firmware=%s OTA state=%d\n", running->label, FIRMWARE_VERSION, static_cast<int>(state));
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
      const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
      Serial.printf("Ludant: startup health confirmation result=%s\n", esp_err_to_name(result));
    }
  }
  preferences.putString("ota_floor", FIRMWARE_VERSION);
}

void setup() {
  Serial.begin(115200);
  // Give the USB CDC/JTAG monitor time to attach before the first diagnostic
  // lines are emitted. The runtime remains usable when no monitor is open.
  delay(250);
  Serial.setDebugOutput(true);
  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
  otaBootStartedAt = millis();
  Serial.printf("Ludant: OTA authorization requires BOOT; development window=%s\n",
#ifdef LUDANT_OTA_DEVELOPMENT_WINDOW_SECONDS
                String(LUDANT_OTA_DEVELOPMENT_WINDOW_SECONDS).c_str()
#else
                "disabled"
#endif
  );
  Serial.printf("Ludant: boot firmware=%s protocol=%u resetReason=%d freeHeap=%u\n",
                FIRMWARE_VERSION, PROTOCOL_VERSION, static_cast<int>(esp_reset_reason()), ESP.getFreeHeap());
  responseQueue = xQueueCreate(4, sizeof(OutboundStatus));
  telemetryQueue = xQueueCreate(8, sizeof(OutboundStatus));
  binaryTelemetryQueue = xQueueCreate(16, sizeof(BinaryTelemetryPacket));
  controlQueue = xQueueCreate(2, sizeof(InboundCommand));
  otaDataQueue = xQueueCreate(OTA_DATA_QUEUE_CAPACITY, sizeof(OtaDataChunk));
  if (responseQueue == nullptr || telemetryQueue == nullptr || binaryTelemetryQueue == nullptr ||
      controlQueue == nullptr || otaDataQueue == nullptr) {
    Serial.println("Ludant: queue allocation failed");
    return;
  }
  logQueueState("queues initialized");
  preferences.begin("ludant", false);
  deviceId = preferences.getString("device_id", "");
  if (deviceId.isEmpty()) {
    uint64_t chip = ESP.getEfuseMac();
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04X%08X", static_cast<uint16_t>(chip >> 32), static_cast<uint32_t>(chip));
    deviceId = String("esp32s3-") + buffer;
    preferences.putString("device_id", deviceId);
  }
  if (!loadModules()) {
    Serial.println("Ludant: persisted module configuration invalid; falling back to empty configuration");
    moduleCount = 0;
    persistModuleList(modules, moduleCount);
  }
  Serial.printf("Ludant: configuration loaded; modules=%u revision=%llu hash=%s\n",
                moduleCount, static_cast<unsigned long long>(configRevision), configHash.c_str());
  BLEDevice::init("Ludant");
  BLEDevice::setMTU(517);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService* service = server->createService(SERVICE_UUID);
  BLECharacteristic* control = service->createCharacteristic(CONTROL_UUID, BLECharacteristic::PROPERTY_WRITE);
  control->setCallbacks(new ControlCallbacks());
  BLECharacteristic* data = service->createCharacteristic(DATA_UUID, BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_WRITE);
  data->setCallbacks(new DataCallbacks());
  statusCharacteristic = service->createCharacteristic(STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  deviceInfoCharacteristic = service->createCharacteristic(DEVICE_INFO_UUID, BLECharacteristic::PROPERTY_READ);
  telemetryCharacteristic = service->createCharacteristic(TELEMETRY_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  deviceInfoCharacteristic->setCallbacks(new DeviceInfoCallbacks());
  statusCharacteristic->setValue("READY");
  const String deviceInfo = deviceInfoJson();
  deviceInfoCharacteristic->setValue(deviceInfo);
  Serial.printf("Ludant: device-info initialized; bytes=%u\n", deviceInfo.length());
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  Serial.println(String("Ludant module firmware ready, deviceId=") + deviceId);
  confirmRunningApplication();
  logQueueState("setup complete");
}

void loop() {
  if (lastHeartbeatAt == 0 || millis() - lastHeartbeatAt >= 5000) {
    lastHeartbeatAt = millis();
    logQueueState("heartbeat");
  }
  processControlQueue();
  processOtaDataQueue();
  processStatusQueue();
  processBinaryTelemetryQueue();
  if (telemetryRunning && static_cast<int32_t>(millis() - telemetryNextAt) >= 0) {
    emitTelemetry();
    telemetryNextAt = millis() + telemetryIntervalMs;
  }
  delay(10);
}
