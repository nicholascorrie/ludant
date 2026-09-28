#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "device_info.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "ota_manager.hpp"
#include "module_manager.hpp"

class BleOtaServer {
public:
    BleOtaServer(OtaManager& ota_manager, const DeviceInfo& device_info, ModuleManager& module_manager);

    esp_err_t start();
    void handleStatus(const char* message);
    void handleDisconnect();

    // Kept public for the static NimBLE GATT callback thunk.
    static int gattAccessCallback(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt* ctxt, void* arg);

private:
    struct QueuedStatus;
    static void statusCallback(const char* message, void* context);
    static void moduleCallback(const char* message, void* context);
    static void telemetryCallback(const ludant::BinaryTelemetryPacket& packet, void* context);
    static void statusTask(void* argument);
    static void telemetryTask(void* argument);
    static void gattRegisterCallback(struct ble_gatt_register_ctxt* ctxt, void* arg);
    static void onBleReset(int reason);
    static void onBleSync();
    static int gapEvent(struct ble_gap_event* event, void* arg);
    static void restartTask(void* argument);

    int handleRead(uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt);
    int handleWrite(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt);
    int handleControlWrite(struct ble_gatt_access_ctxt* ctxt);
    int handleCompleteControlWrite(const char* message, size_t length);
    int handleDataWrite(struct ble_gatt_access_ctxt* ctxt);
    bool processCommandFrame(const uint8_t* data, size_t length);
    void resetCommandFrame();
    bool advertise();
    void notifyStatus(const char* message);
    void transmitStatus(const QueuedStatus& status);
    void notifyTelemetry(const ludant::BinaryTelemetryPacket& packet);
    void scheduleRestart();

    OtaManager& ota_manager_;
    const DeviceInfo& device_info_;
    ModuleManager& module_manager_;
    uint16_t connection_handle_{BLE_HS_CONN_HANDLE_NONE};
    bool status_notifications_enabled_{false};
    bool telemetry_notifications_enabled_{false};
    bool restart_pending_{false};
    SemaphoreHandle_t advertising_ready_{nullptr};
    char last_status_[160]{"IDLE"};
    static constexpr size_t kMaxStatusMessageLength = 2048;
    struct QueuedStatus {
        uint16_t connection_handle;
        size_t length;
        char data[kMaxStatusMessageLength + 1];
    };
    QueueHandle_t status_queue_{nullptr};
    TaskHandle_t status_task_{nullptr};
    uint16_t next_status_message_id_{0};
    QueueHandle_t telemetry_queue_{nullptr};
    TaskHandle_t telemetry_task_{nullptr};

    struct CommandFrameAssembly {
        std::string data;
        uint16_t message_length{0};
        uint16_t next_chunk{0};
        uint16_t total_chunks{0};
        uint16_t message_id{0};
        uint16_t checksum{0};
        uint32_t updated_at_ms{0};
        bool active{false};
    } command_frame_;

    static BleOtaServer* instance_;
};
