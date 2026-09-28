#include "ble_ota_server.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

extern "C" void ble_store_config_init(void);

namespace {
constexpr char TAG[] = "ble_ota";
// Keep the complete service UUID and the name inside the legacy 31-byte
// advertising payload so discovery works without relying on scan response.
constexpr char kDeviceName[] = "Ludant";
constexpr size_t kMaxControlJson = 8192;
constexpr size_t kMaxFramedControlJson = 2047;
constexpr size_t kStatusFrameHeaderLength = 16;
constexpr size_t kStatusFrameMaximumLength = 517;
constexpr size_t kMaxGattAttributeValueLength = 512;
// Keep the server-side copy bounded to the same size accepted by the OTA
// manager's queue. This prevents a large ATT write from being split into a
// callback-sized buffer that the manager would reject.
constexpr size_t kCopyChunkSize = 512;
constexpr uint8_t kCommandFrameMagic = 0xC2;
constexpr uint8_t kCommandFrameVersion = 1;
constexpr size_t kCommandFrameHeaderLength = 12;
constexpr size_t kBinaryTelemetryMaxPayload = ludant::kBinaryTelemetryMaxPayloadLength;

// NimBLE's BLE_UUID128_INIT takes the UUID bytes in little-endian order.
// These bytes render as 7A910000-4C5E-4A9B-8F23-91F4A7D10000.
static const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xd1, 0xa7, 0xf4, 0x91, 0x23, 0x8f,
    0x9b, 0x4a, 0x5e, 0x4c, 0x00, 0x00, 0x91, 0x7a);
static const ble_uuid128_t kControlUuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xd1, 0xa7, 0xf4, 0x91, 0x23, 0x8f,
    0x9b, 0x4a, 0x5e, 0x4c, 0x01, 0x00, 0x91, 0x7a);
static const ble_uuid128_t kDataUuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xd1, 0xa7, 0xf4, 0x91, 0x23, 0x8f,
    0x9b, 0x4a, 0x5e, 0x4c, 0x02, 0x00, 0x91, 0x7a);
static const ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xd1, 0xa7, 0xf4, 0x91, 0x23, 0x8f,
    0x9b, 0x4a, 0x5e, 0x4c, 0x03, 0x00, 0x91, 0x7a);
static const ble_uuid128_t kDeviceInfoUuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xd1, 0xa7, 0xf4, 0x91, 0x23, 0x8f,
    0x9b, 0x4a, 0x5e, 0x4c, 0x04, 0x00, 0x91, 0x7a);
static const ble_uuid128_t kTelemetryUuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xd1, 0xa7, 0xf4, 0x91, 0x23, 0x8f,
    0x9b, 0x4a, 0x5e, 0x4c, 0x05, 0x00, 0x91, 0x7a);

static uint16_t control_handle = 0;
static uint16_t data_handle = 0;
static uint16_t status_handle = 0;
static uint16_t device_info_handle = 0;
static uint16_t telemetry_handle = 0;

uint16_t crc16Bytes(const uint8_t* data, size_t length);
void writeBase36(char* destination, size_t width, uint32_t value);
size_t utf8SafeChunkLength(const char* message, size_t offset, size_t remaining, size_t capacity);

static int accessCallback(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* arg) {
    return BleOtaServer::gattAccessCallback(conn_handle, attr_handle, ctxt, arg);
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &kControlUuid.u,
                .access_cb = accessCallback,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &control_handle,
            },
            {
                .uuid = &kDataUuid.u,
                .access_cb = accessCallback,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &data_handle,
            },
            {
                .uuid = &kStatusUuid.u,
                .access_cb = accessCallback,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &status_handle,
            },
            {
                .uuid = &kDeviceInfoUuid.u,
                .access_cb = accessCallback,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &device_info_handle,
            },
            {
                .uuid = &kTelemetryUuid.u,
                .access_cb = accessCallback,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &telemetry_handle,
            },
            { 0 },
        },
    },
    { 0 },
};
}

BleOtaServer* BleOtaServer::instance_ = nullptr;

BleOtaServer::BleOtaServer(OtaManager& ota_manager, const DeviceInfo& device_info, ModuleManager& module_manager)
    : ota_manager_(ota_manager), device_info_(device_info), module_manager_(module_manager) {
    instance_ = this;
    ota_manager_.setStatusCallback(statusCallback, this);
    module_manager_.setOutputCallback(moduleCallback, this);
    module_manager_.setTelemetryCallback(telemetryCallback, this);
}

esp_err_t BleOtaServer::start() {
    if (!ota_manager_.startDataWorker()) {
        ESP_LOGE(TAG, "Could not start OTA data worker");
        return ESP_ERR_NO_MEM;
    }
    advertising_ready_ = xSemaphoreCreateBinary();
    if (advertising_ready_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    telemetry_queue_ = xQueueCreate(16, sizeof(ludant::BinaryTelemetryPacket));
    status_queue_ = xQueueCreate(4, sizeof(QueuedStatus*));
    if (telemetry_queue_ == nullptr || status_queue_ == nullptr ||
        xTaskCreate(telemetryTask, "ludant_binary_telemetry", 3072, this, 4, &telemetry_task_) != pdPASS ||
        xTaskCreate(statusTask, "ludant_ble_status", 4096, this, 5, &status_task_) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t nimble_err = nimble_port_init();
    if (nimble_err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(nimble_err));
        return nimble_err;
    }

    ble_hs_cfg.reset_cb = onBleReset;
    ble_hs_cfg.sync_cb = onBleSync;
    ble_hs_cfg.gatts_register_cb = gattRegisterCallback;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(kDeviceName);

    int rc = ble_gatts_count_cfg(services);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count GATT configuration: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(services);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT services: %d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init([](void*) { nimble_port_run(); });
    if (xSemaphoreTake(advertising_ready_, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "BLE advertising did not become ready");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void BleOtaServer::statusCallback(const char* message, void* context) {
    static_cast<BleOtaServer*>(context)->handleStatus(message);
}

void BleOtaServer::moduleCallback(const char* message, void* context) {
    static_cast<BleOtaServer*>(context)->handleStatus(message);
}

void BleOtaServer::telemetryCallback(const ludant::BinaryTelemetryPacket& packet, void* context) {
    static_cast<BleOtaServer*>(context)->notifyTelemetry(packet);
}

void BleOtaServer::statusTask(void* argument) {
    auto* server = static_cast<BleOtaServer*>(argument);
    QueuedStatus* status = nullptr;
    while (true) {
        if (server->status_queue_ == nullptr ||
            xQueueReceive(server->status_queue_, &status, portMAX_DELAY) != pdTRUE || status == nullptr) {
            continue;
        }
        if (server->status_notifications_enabled_ &&
            status->connection_handle == server->connection_handle_) {
            server->transmitStatus(*status);
        }
        delete status;
        status = nullptr;
    }
}

void BleOtaServer::telemetryTask(void* argument) {
    auto* server = static_cast<BleOtaServer*>(argument);
    ludant::BinaryTelemetryPacket packet{};
    while (true) {
        if (server->telemetry_queue_ == nullptr ||
            xQueueReceive(server->telemetry_queue_, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!server->telemetry_notifications_enabled_ ||
            server->connection_handle_ == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }
        uint8_t payload[kBinaryTelemetryMaxPayload]{};
        const size_t payload_length = ludant::encodeBinaryTelemetryPayload(packet, payload, sizeof(payload));
        if (payload_length == 0 || payload_length > UINT16_MAX) {
            ESP_LOGW(TAG, "Could not encode binary telemetry packet: fields=%u", packet.field_count);
            continue;
        }
        size_t offset = 0;
        bool first = true;
        while (offset < payload_length) {
            uint8_t frame[ludant::kBinaryTelemetryFrameLength]{};
            const size_t header_length = first ? 11 : 7;
            const size_t chunk_length = std::min(ludant::kBinaryTelemetryFrameLength - header_length,
                                                payload_length - offset);
            frame[0] = ludant::kBinaryTelemetryFrameMagic;
            frame[1] = ludant::kBinaryTelemetryVersion;
            frame[2] = (first ? ludant::kBinaryTelemetryFrameStart : 0) |
                       (offset + chunk_length == payload_length ? ludant::kBinaryTelemetryFrameEnd : 0);
            frame[3] = static_cast<uint8_t>(packet.sequence & 0xFF);
            frame[4] = static_cast<uint8_t>(packet.sequence >> 8);
            if (first) {
                frame[5] = static_cast<uint8_t>(packet.uptime_ms & 0xFF);
                frame[6] = static_cast<uint8_t>((packet.uptime_ms >> 8) & 0xFF);
                frame[7] = static_cast<uint8_t>((packet.uptime_ms >> 16) & 0xFF);
                frame[8] = static_cast<uint8_t>((packet.uptime_ms >> 24) & 0xFF);
                frame[9] = static_cast<uint8_t>(payload_length & 0xFF);
                frame[10] = static_cast<uint8_t>(payload_length >> 8);
            } else {
                frame[5] = static_cast<uint8_t>(offset & 0xFF);
                frame[6] = static_cast<uint8_t>(offset >> 8);
            }
            std::memcpy(frame + header_length, payload + offset, chunk_length);
            struct os_mbuf* buffer = ble_hs_mbuf_from_flat(frame, header_length + chunk_length);
            if (buffer == nullptr) {
                ESP_LOGW(TAG, "Could not allocate binary telemetry notification buffer");
                break;
            }
            const int rc = ble_gatts_notify_custom(server->connection_handle_, telemetry_handle, buffer);
            if (rc != 0) {
                ESP_LOGW(TAG, "Binary telemetry notification failed: %d", rc);
                break;
            }
            offset += chunk_length;
            first = false;
        }
    }
}

void BleOtaServer::handleStatus(const char* message) {
    if (message == nullptr) {
        return;
    }
    const size_t message_length = std::strlen(message);
    const size_t saved_length = std::min(message_length, sizeof(last_status_) - 1);
    if (message_length < sizeof(last_status_)) {
        std::memcpy(last_status_, message, saved_length);
        last_status_[saved_length] = '\0';
    } else {
        // Do not replay a truncated JSON response to a reconnecting client.
        std::strncpy(last_status_, "RESPONSE_AVAILABLE", sizeof(last_status_) - 1);
        last_status_[sizeof(last_status_) - 1] = '\0';
    }
    if (message_length > kMaxStatusMessageLength) {
        ESP_LOGE(TAG, "Status message exceeds framing limit: bytes=%u maximum=%u",
                 static_cast<unsigned>(message_length), static_cast<unsigned>(kMaxStatusMessageLength));
        return;
    }
    notifyStatus(message);
    if (std::strcmp(message, "SUCCESS") == 0) {
        scheduleRestart();
    }
}

void BleOtaServer::notifyStatus(const char* message) {
    if (message == nullptr || !status_notifications_enabled_ ||
        connection_handle_ == BLE_HS_CONN_HANDLE_NONE || status_queue_ == nullptr) {
        return;
    }

    const size_t message_length = std::strlen(message);
    if (message_length == 0 || message_length > kMaxStatusMessageLength) {
        ESP_LOGE(TAG, "Cannot queue status notification: bytes=%u maximum=%u",
                 static_cast<unsigned>(message_length), static_cast<unsigned>(kMaxStatusMessageLength));
        return;
    }

    auto* status = new (std::nothrow) QueuedStatus{};
    if (status == nullptr) {
        ESP_LOGE(TAG, "Could not allocate status notification");
        return;
    }
    status->connection_handle = connection_handle_;
    status->length = message_length;
    std::memcpy(status->data, message, message_length);
    status->data[message_length] = '\0';
    if (xQueueSend(status_queue_, &status, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Status notification queue is full; dropping message bytes=%u",
                 static_cast<unsigned>(message_length));
        delete status;
    }
}

void BleOtaServer::transmitStatus(const QueuedStatus& status) {
    if (status.length == 0 || status.length > kMaxStatusMessageLength) return;

    const uint16_t mtu = ble_att_mtu(status.connection_handle);
    if (mtu <= 3 + kStatusFrameHeaderLength) {
        ESP_LOGW(TAG, "BLE MTU too small for status framing: mtu=%u", mtu);
        return;
    }
    const size_t payload_capacity = std::min<size_t>(
        mtu - 3 - kStatusFrameHeaderLength,
        kStatusFrameMaximumLength - kStatusFrameHeaderLength);

    uint32_t total_chunks = 0;
    for (size_t offset = 0; offset < status.length; ++total_chunks) {
        const size_t chunk_length = utf8SafeChunkLength(
            status.data, offset, status.length - offset, payload_capacity);
        if (chunk_length == 0) {
            ESP_LOGE(TAG, "Could not split status at a UTF-8 boundary: offset=%u",
                     static_cast<unsigned>(offset));
            return;
        }
        offset += chunk_length;
    }

    const uint16_t message_id = next_status_message_id_;
    next_status_message_id_ = static_cast<uint16_t>((next_status_message_id_ + 1) % (36 * 36));
    const uint16_t checksum = crc16Bytes(
        reinterpret_cast<const uint8_t*>(status.data), status.length);
    size_t offset = 0;
    uint32_t index = 0;
    while (offset < status.length) {
        if (!status_notifications_enabled_ || connection_handle_ != status.connection_handle) return;

        const size_t chunk_length = utf8SafeChunkLength(
            status.data, offset, status.length - offset, payload_capacity);
        char frame[kStatusFrameMaximumLength]{};
        char message_id_text[3]{};
        char index_text[3]{};
        char total_text[4]{};
        char length_text[4]{};
        char checksum_text[5]{};
        writeBase36(message_id_text, 2, message_id);
        writeBase36(index_text, 2, index);
        writeBase36(total_text, 3, total_chunks);
        writeBase36(length_text, 3, static_cast<uint32_t>(status.length));
        std::snprintf(checksum_text, sizeof(checksum_text), "%04X", checksum);
        std::memcpy(frame, "F2", 2);
        std::memcpy(frame + 2, message_id_text, 2);
        std::memcpy(frame + 4, index_text, 2);
        std::memcpy(frame + 6, total_text, 3);
        std::memcpy(frame + 9, length_text, 3);
        std::memcpy(frame + 12, checksum_text, 4);
        std::memcpy(frame + kStatusFrameHeaderLength, status.data + offset, chunk_length);

        const size_t frame_length = kStatusFrameHeaderLength + chunk_length;
        struct os_mbuf* buffer = ble_hs_mbuf_from_flat(frame, frame_length);
        if (buffer == nullptr) {
            ESP_LOGE(TAG, "Could not allocate status frame: id=%u index=%u/%u",
                     message_id, static_cast<unsigned>(index), static_cast<unsigned>(total_chunks));
            return;
        }
        const int rc = ble_gatts_notify_custom(status.connection_handle, status_handle, buffer);
        if (rc != 0) {
            ESP_LOGW(TAG, "Status frame notify failed: id=%u index=%u/%u rc=%d",
                     message_id, static_cast<unsigned>(index), static_cast<unsigned>(total_chunks), rc);
            return;
        }
        offset += chunk_length;
        ++index;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void BleOtaServer::notifyTelemetry(const ludant::BinaryTelemetryPacket& packet) {
    if (telemetry_queue_ == nullptr) return;
    if (xQueueSend(telemetry_queue_, &packet, 0) == pdTRUE) return;
    ludant::BinaryTelemetryPacket discarded{};
    xQueueReceive(telemetry_queue_, &discarded, 0);
    xQueueSend(telemetry_queue_, &packet, 0);
}

void BleOtaServer::handleDisconnect() {
    status_notifications_enabled_ = false;
    telemetry_notifications_enabled_ = false;
    connection_handle_ = BLE_HS_CONN_HANDLE_NONE;
    if (status_queue_ != nullptr) {
        QueuedStatus* pending = nullptr;
        while (xQueueReceive(status_queue_, &pending, 0) == pdTRUE) {
            delete pending;
            pending = nullptr;
        }
    }
    if (telemetry_queue_ != nullptr) xQueueReset(telemetry_queue_);
    ota_manager_.onDisconnect();
    module_manager_.stopTelemetry();
}

int BleOtaServer::handleRead(uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt) {
    std::string value;
    if (attr_handle == status_handle) {
        value = last_status_;
    } else if (attr_handle == device_info_handle) {
        value = device_info_.json();
    } else {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    if (value.size() > kMaxGattAttributeValueLength) {
        ESP_LOGE(TAG, "GATT read value exceeds attribute limit: handle=%u bytes=%u maximum=%u",
                 attr_handle, static_cast<unsigned>(value.size()),
                 static_cast<unsigned>(kMaxGattAttributeValueLength));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    return os_mbuf_append(ctxt->om, value.data(), value.size()) == 0
        ? 0
        : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int BleOtaServer::handleControlWrite(struct ble_gatt_access_ctxt* ctxt) {
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0 || length > kMaxControlJson) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t first_byte = 0;
    if (os_mbuf_copydata(ctxt->om, 0, 1, &first_byte) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (first_byte == kCommandFrameMagic) {
        std::vector<uint8_t> frame(length);
        if (os_mbuf_copydata(ctxt->om, 0, length, frame.data()) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return processCommandFrame(frame.data(), frame.size())
            ? 0
            : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    std::string message(length, '\0');
    if (os_mbuf_copydata(ctxt->om, 0, length, &message[0]) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return handleCompleteControlWrite(message.c_str(), length);
}

int BleOtaServer::handleCompleteControlWrite(const char* message, size_t length) {
    if (message == nullptr || length == 0 || length > kMaxControlJson) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON* root = cJSON_ParseWithLength(message, length);
    if (root == nullptr) {
        ota_manager_.abort("malformed control JSON");
        handleStatus("ERROR:INVALID_JSON:control message is not valid JSON");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const cJSON* command_item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(command_item) || command_item->valuestring == nullptr) {
        cJSON_Delete(root);
        handleStatus("ERROR:INVALID_COMMAND:command is required");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    bool accepted = false;
    if (std::strcmp(command_item->valuestring, "ota_authorization_status") == 0) {
        if (ota_manager_.isActive()) {
            handleStatus("ERROR:OTA_BUSY:controller commands are disabled during firmware update");
            accepted = false;
        } else {
            handleStatus(ota_manager_.authorizationAllowed() ? "AUTHORIZATION_READY" : "WAITING_FOR_BOOT");
            accepted = true;
        }
    } else if (std::strcmp(command_item->valuestring, "begin") == 0) {
        const cJSON* size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
        const cJSON* sha_item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
        const cJSON* version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON* product_item = cJSON_GetObjectItemCaseSensitive(root, "product");
        const cJSON* hardware_item = cJSON_GetObjectItemCaseSensitive(root, "hardware");
        const cJSON* ota_protocol_item = cJSON_GetObjectItemCaseSensitive(root, "otaProtocol");
        const cJSON* signature_item = cJSON_GetObjectItemCaseSensitive(root, "signature");
        const cJSON* algorithm_item = signature_item == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(signature_item, "algorithm");
        const cJSON* value_item = signature_item == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(signature_item, "value");
        if (cJSON_IsNumber(size_item) && cJSON_IsString(sha_item) && cJSON_IsString(version_item) &&
            cJSON_IsString(product_item) && cJSON_IsString(hardware_item) && cJSON_IsNumber(ota_protocol_item) &&
            cJSON_IsString(algorithm_item) && cJSON_IsString(value_item) &&
            size_item->valuedouble >= 1.0 && size_item->valuedouble <= 4294967295.0 &&
            size_item->valuedouble == static_cast<double>(static_cast<uint32_t>(size_item->valuedouble))) {
            accepted = ota_manager_.begin(
                static_cast<uint32_t>(size_item->valuedouble),
                sha_item->valuestring,
                version_item->valuestring,
                product_item->valuestring,
                hardware_item->valuestring,
                static_cast<uint8_t>(ota_protocol_item->valuedouble),
                std::strcmp(algorithm_item->valuestring, "ed25519") == 0 ? value_item->valuestring : "");
        } else {
            handleStatus("ERROR:INVALID_BEGIN:begin requires integer size, sha256, and version");
        }
    } else if (std::strcmp(command_item->valuestring, "end") == 0) {
        accepted = ota_manager_.finish();
    } else if (std::strcmp(command_item->valuestring, "abort") == 0) {
        ota_manager_.abort("client requested abort");
        accepted = true;
    } else {
        if (ota_manager_.isActive()) {
            handleStatus("ERROR:OTA_BUSY:controller commands are disabled during firmware update");
            accepted = false;
        } else {
            accepted = module_manager_.handleCommand(message, length);
        }
    }

    cJSON_Delete(root);
    return accepted ? 0 : BLE_ATT_ERR_UNLIKELY;
}

namespace {
uint16_t readLittleEndian16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

uint16_t crc16Bytes(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                                 : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

void writeBase36(char* destination, size_t width, uint32_t value) {
    static constexpr char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (size_t index = width; index > 0; --index) {
        destination[index - 1] = digits[value % 36];
        value /= 36;
    }
    destination[width] = '\0';
}

size_t utf8SafeChunkLength(const char* message, size_t offset, size_t remaining, size_t capacity) {
    size_t length = std::min(remaining, capacity);
    while (length > 0 && offset + length < offset + remaining &&
           (static_cast<uint8_t>(message[offset + length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}
}

void BleOtaServer::resetCommandFrame() {
    command_frame_ = CommandFrameAssembly{};
}

bool BleOtaServer::processCommandFrame(const uint8_t* data, size_t length) {
    const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (command_frame_.active && now_ms - command_frame_.updated_at_ms >= 5000) {
        ESP_LOGW(TAG, "Command frame assembly expired: id=%u received=%u/%u",
                 command_frame_.message_id, command_frame_.next_chunk,
                 command_frame_.total_chunks);
        resetCommandFrame();
    }
    if (data == nullptr || length < kCommandFrameHeaderLength ||
        data[0] != kCommandFrameMagic || data[1] != kCommandFrameVersion) {
        ESP_LOGW(TAG, "Invalid command frame header: bytes=%u magic=0x%02X version=%u",
                 static_cast<unsigned>(length), data != nullptr && length > 0 ? data[0] : 0,
                 data != nullptr && length > 1 ? data[1] : 0);
        resetCommandFrame();
        return false;
    }

    const uint16_t message_id = readLittleEndian16(data + 2);
    const uint16_t index = readLittleEndian16(data + 4);
    const uint16_t total_chunks = readLittleEndian16(data + 6);
    const uint16_t message_length = readLittleEndian16(data + 8);
    const uint16_t checksum = readLittleEndian16(data + 10);
    const size_t payload_length = length - kCommandFrameHeaderLength;
    if (total_chunks == 0 || index >= total_chunks || message_length == 0 ||
        message_length > kMaxFramedControlJson || payload_length == 0 ||
        payload_length > message_length) {
        ESP_LOGW(TAG, "Invalid command frame metadata: id=%u index=%u/%u messageBytes=%u payloadBytes=%u",
                 message_id, index, total_chunks, message_length,
                 static_cast<unsigned>(payload_length));
        resetCommandFrame();
        return false;
    }

    if (!command_frame_.active) {
        if (index != 0) {
            ESP_LOGW(TAG, "Command frame started out of order: id=%u index=%u", message_id, index);
            return false;
        }
        command_frame_.active = true;
        command_frame_.message_id = message_id;
        command_frame_.message_length = message_length;
        command_frame_.total_chunks = total_chunks;
        command_frame_.checksum = checksum;
        command_frame_.data.reserve(message_length);
    } else if (command_frame_.message_id != message_id ||
               command_frame_.message_length != message_length ||
               command_frame_.total_chunks != total_chunks ||
               command_frame_.checksum != checksum) {
        ESP_LOGW(TAG, "Command frame metadata changed mid-message: activeId=%u receivedId=%u",
                 command_frame_.message_id, message_id);
        resetCommandFrame();
        return false;
    }

    if (index != command_frame_.next_chunk ||
        command_frame_.data.size() + payload_length > message_length) {
        ESP_LOGW(TAG, "Command frame out of order or too large: id=%u index=%u expected=%u assembled=%u payload=%u",
                 message_id, index, command_frame_.next_chunk,
                 static_cast<unsigned>(command_frame_.data.size()),
                 static_cast<unsigned>(payload_length));
        resetCommandFrame();
        return false;
    }

    command_frame_.data.append(reinterpret_cast<const char*>(data + kCommandFrameHeaderLength), payload_length);
    command_frame_.next_chunk++;
    command_frame_.updated_at_ms = now_ms;
    if (index == 0 || index + 1 == total_chunks) {
        ESP_LOGI(TAG, "Command frame received: id=%u frame=%u/%u bytes=%u assembled=%u/%u",
                 message_id, index + 1, total_chunks, static_cast<unsigned>(length),
                 static_cast<unsigned>(command_frame_.data.size()), message_length);
    }
    if (command_frame_.next_chunk < command_frame_.total_chunks) {
        return true;
    }

    const auto* assembled = reinterpret_cast<const uint8_t*>(command_frame_.data.data());
    const uint16_t actual_checksum = crc16Bytes(assembled, command_frame_.data.size());
    if (command_frame_.data.size() != command_frame_.message_length ||
        actual_checksum != command_frame_.checksum) {
        ESP_LOGW(TAG, "Command frame checksum/length failed: id=%u assembled=%u expected=%u crc=%04X expected=%04X",
                 message_id, static_cast<unsigned>(command_frame_.data.size()),
                 command_frame_.message_length, actual_checksum, command_frame_.checksum);
        resetCommandFrame();
        return false;
    }

    ESP_LOGI(TAG, "Command frame reassembled: id=%u chunks=%u bytes=%u crc=%04X",
             message_id, command_frame_.total_chunks,
             static_cast<unsigned>(command_frame_.data.size()), command_frame_.checksum);
    const int result = handleCompleteControlWrite(command_frame_.data.c_str(), command_frame_.data.size());
    resetCommandFrame();
    return result == 0;
}

int BleOtaServer::handleDataWrite(struct ble_gatt_access_ctxt* ctxt) {
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    // Copy the complete ATT write in bounded pieces. Flash writes run on the
    // OTA worker so this NimBLE callback remains responsive to acknowledgements.
    uint8_t copy_buffer[kCopyChunkSize];
    uint16_t offset = 0;
    while (offset < length) {
        const uint16_t chunk_length = std::min<uint16_t>(kCopyChunkSize, length - offset);
        if (os_mbuf_copydata(ctxt->om, offset, chunk_length, copy_buffer) != 0) {
            ota_manager_.abort("could not read BLE data buffer");
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (!ota_manager_.enqueueData(copy_buffer, chunk_length)) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        offset += chunk_length;
    }
    return 0;
}

int BleOtaServer::handleWrite(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt* ctxt) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || conn_handle != connection_handle_) {
        ESP_LOGW(TAG, "Rejected write from inactive BLE connection: received=%u active=%u",
                 conn_handle, connection_handle_);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (attr_handle == control_handle) {
        return handleControlWrite(ctxt);
    }
    if (attr_handle == data_handle) {
        return handleDataWrite(ctxt);
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

int BleOtaServer::gattAccessCallback(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt* ctxt, void*) {
    if (instance_ == nullptr || ctxt == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return instance_->handleRead(attr_handle, ctxt);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return instance_->handleWrite(conn_handle, attr_handle, ctxt);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void BleOtaServer::gattRegisterCallback(struct ble_gatt_register_ctxt* ctxt, void*) {
    if (ctxt == nullptr) {
        return;
    }
    char uuid_string[BLE_UUID_STR_LEN]{};
    if (ctxt->op == BLE_GATT_REGISTER_OP_SVC) {
        ESP_LOGI(TAG, "registered service %s handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_string), ctxt->svc.handle);
    } else if (ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        ESP_LOGI(TAG, "registered characteristic %s value_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_string), ctxt->chr.val_handle);
    }
}

void BleOtaServer::onBleReset(int reason) {
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

void BleOtaServer::onBleSync() {
    if (instance_ != nullptr) {
        if (instance_->advertise() && instance_->advertising_ready_ != nullptr) {
            xSemaphoreGive(instance_->advertising_ready_);
        }
    }
}

bool BleOtaServer::advertise() {
    uint8_t own_address_type = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not infer BLE address type: %d", rc);
        return false;
    }

    struct ble_hs_adv_fields fields{};
    fields.name = reinterpret_cast<uint8_t*>(const_cast<char*>(kDeviceName));
    fields.name_len = std::strlen(kDeviceName);
    fields.name_is_complete = 1;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set advertisement fields: %d", rc);
        return false;
    }

    struct ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_address_type, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start BLE advertising: %d", rc);
        return false;
    }
    ESP_LOGI(TAG, "BLE advertising as %s", kDeviceName);
    return true;
}

int BleOtaServer::gapEvent(struct ble_gap_event* event, void*) {
    if (instance_ == nullptr || event == nullptr) {
        return 0;
    }
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                instance_->connection_handle_ = event->connect.conn_handle;
                instance_->status_notifications_enabled_ = false;
                instance_->telemetry_notifications_enabled_ = false;
                // Telemetry is scoped to a BLE session. Reset it even if the
                // previous client was terminated without a clean disconnect
                // callback reaching the controller.
                instance_->resetCommandFrame();
                instance_->module_manager_.stopTelemetry();
                const int security_rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (security_rc != 0) {
                    ESP_LOGW(TAG, "BLE security negotiation could not start: status=%d", security_rc);
                }
                ESP_LOGI(TAG, "BLE client connected: handle=%d", event->connect.conn_handle);
            } else {
                ESP_LOGW(TAG, "BLE connection failed: status=%d", event->connect.status);
                instance_->advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE client disconnected: reason=%d", event->disconnect.reason);
            instance_->handleDisconnect();
            instance_->advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            instance_->advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == status_handle) {
                instance_->status_notifications_enabled_ = event->subscribe.cur_notify != 0;
                ESP_LOGI(TAG, "Status notifications %s",
                         instance_->status_notifications_enabled_ ? "enabled" : "disabled");
                if (instance_->status_notifications_enabled_) {
                    instance_->notifyStatus(instance_->last_status_);
                }
            } else if (event->subscribe.attr_handle == telemetry_handle) {
                instance_->telemetry_notifications_enabled_ = event->subscribe.cur_notify != 0;
                ESP_LOGI(TAG, "Binary telemetry notifications %s",
                         instance_->telemetry_notifications_enabled_ ? "enabled" : "disabled");
            }
            break;
        default:
            break;
    }
    return 0;
}

void BleOtaServer::scheduleRestart() {
    if (restart_pending_) {
        return;
    }
    restart_pending_ = true;
    if (xTaskCreate(restartTask, "ota_restart", 2048, this, 5, nullptr) != pdPASS) {
        restart_pending_ = false;
        ESP_LOGE(TAG, "Could not schedule OTA reboot");
    }
}

void BleOtaServer::restartTask(void* argument) {
    auto* server = static_cast<BleOtaServer*>(argument);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Rebooting into verified OTA image");
    esp_restart();
    vTaskDelete(nullptr);
    (void)server;
}
