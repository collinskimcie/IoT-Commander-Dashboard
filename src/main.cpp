#include <Arduino.h>
#include <esp_system.h>
#include <ArduinoJson.h>
#include <esp_display_panel.hpp>

#include "app/ConnectivityService.h"
#include "app/BackendService.h"
#include "app/MqttService.h"
#include "app/NotificationCenter.h"
#include "app/SettingsStore.h"
#include "app/TimeService.h"
#include "app/SlowGuard.h"
#include "platform/LvglPort.h"
#include "ui/UiManager.h"

#include <cstring>
#include <array>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr size_t kMaxUsbCommandLength = 512;
constexpr char kBackendConfigPrefix[] = "CONFIGURE_BACKEND ";

esp_panel::board::Board board;
ConnectivityService connectivity;
NotificationCenter notifications;
SettingsStore settings;
MqttService mqtt;
BackendService backend;
TimeService timeService;
UiManager ui(connectivity, mqtt, notifications, settings, timeService);
bool previousWifiConnected = false;
bool previousMqttConnected = false;
char usbCommand[kMaxUsbCommandLength];
size_t usbCommandLength = 0;
bool usbCommandOverflow = false;

struct NetworkSnapshot {
    TerminalSettings terminal;
    MqttSettings mqtt;
    bool wifiConnected = false;
    uint32_t revision = 0;
};
NetworkSnapshot networkSnapshot;
SemaphoreHandle_t snapshotLock = nullptr;

const char* resetReasonName()
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "other";
    }
}

const char* backendStatusName()
{
    switch (backendStatus()) {
    case BackendStatus::NotConfigured: return "not-configured";
    case BackendStatus::Connecting:    return "connecting";
    case BackendStatus::Ok:            return "ok";
    case BackendStatus::Retrying:      return "retrying";
    case BackendStatus::AuthFailed:    return "auth-failed";
    }
    return "?";
}

void refreshBackendSnapshot(bool wifiConnected)
{
    xSemaphoreTake(snapshotLock, portMAX_DELAY);
    networkSnapshot.terminal = settings.terminalSettings();
    networkSnapshot.mqtt = settings.mqttSettings();
    networkSnapshot.wifiConnected = wifiConnected;
    ++networkSnapshot.revision;
    xSemaphoreGive(snapshotLock);
}

// One task owns all blocking network work (MQTT + backend), so LVGL never waits on it.
// A single task also avoids a second large stack, which internal RAM cannot spare.
void networkTask(void*)
{
    static NetworkSnapshot local;
    uint32_t seenRevision = 0;
    uint32_t lastReportAt = 0;
    for (;;) {
        xSemaphoreTake(snapshotLock, portMAX_DELAY);
        if (networkSnapshot.revision != seenRevision) {
            local = networkSnapshot;
            seenRevision = networkSnapshot.revision;
        }
        xSemaphoreGive(snapshotLock);

        mqtt.service(local.mqtt);
        backend.update(
            local.terminal,
            local.wifiConnected,
            mqtt.readingsUnsafe(),
            local.mqtt.unit,
            mqtt.readingRevision());

        if (millis() - lastReportAt >= 30000) {
            lastReportAt = millis();
            log_i("net task: stack free=%u heap free=%u maxblock=%u psram free=%u/%u "
                  "wifi=%s mqtt=%s backend=%s reset=%s",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()),
                  static_cast<unsigned>(ESP.getFreePsram()),
                  static_cast<unsigned>(ESP.getPsramSize()),
                  local.wifiConnected ? "up" : "down",
                  mqtt.connected() ? "up" : "down",
                  backendStatusName(),
                  resetReasonName());
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void haltWithError(const char* message)
{
    Serial.printf("FATAL: %s\n", message);
    while (true) {
        delay(1000);
    }
}

void configureBackendFromUsb(const char* command)
{
    if (std::strcmp(command, "STATUS") == 0) {
        const TerminalSettings& terminalSettings = settings.terminalSettings();
        Serial.printf(
            "USB status: wifi=%s ip=%s backend=%s timezone=%s time=%s synchronized=%s\n",
            connectivity.wifiConnected() ? "connected" : "disconnected",
            connectivity.ipAddress().c_str(),
            !terminalSettings.backendUrl.isEmpty() && !terminalSettings.backendApiKey.isEmpty()
                ? "configured"
                : "not-configured",
            terminalSettings.timezone.c_str(),
            timeService.localTime().c_str(),
            timeService.synchronized() ? "yes" : "no");
        return;
    }

    if (std::strncmp(command, kBackendConfigPrefix, sizeof(kBackendConfigPrefix) - 1) != 0) {
        Serial.println("ERROR: Unknown USB configuration command");
        return;
    }

    JsonDocument document;
    const DeserializationError error = deserializeJson(
        document,
        command + sizeof(kBackendConfigPrefix) - 1);
    if (error ||
        !document["backendUrl"].is<const char*>() ||
        !document["apiKey"].is<const char*>()) {
        Serial.println("ERROR: USB configuration must include backendUrl and apiKey strings");
        return;
    }
    const JsonVariantConst timezoneValue = document["timezone"];
    if (!timezoneValue.isUnbound() && !timezoneValue.is<const char*>()) {
        Serial.println("ERROR: USB timezone must be a string");
        return;
    }

    const char* backendUrl = document["backendUrl"];
    const char* apiKey = document["apiKey"];
    const char* timezone = !timezoneValue.isUnbound()
        ? timezoneValue.as<const char*>()
        : nullptr;
    const size_t apiKeyLength = std::strlen(apiKey);
    const bool supportedScheme =
        std::strncmp(backendUrl, "http://", 7) == 0 ||
        std::strncmp(backendUrl, "https://", 8) == 0;
    if (!supportedScheme ||
        apiKeyLength < 32 ||
        apiKeyLength > 64 ||
        (timezone != nullptr && std::strlen(timezone) > 40)) {
        Serial.println("ERROR: Backend URL scheme or API key length is invalid");
        return;
    }

    TerminalSettings updated = settings.terminalSettings();
    updated.backendUrl = backendUrl;
    updated.backendApiKey = apiKey;
    if (timezone != nullptr) {
        updated.timezone = timezone;
    }
    if (!settings.setTerminalSettings(updated)) {
        Serial.println("ERROR: Could not save backend settings");
        return;
    }
    Serial.println("USB backend and timezone settings saved");
}

void updateUsbProvisioning()
{
    while (Serial.available() > 0) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            if (usbCommandOverflow) {
                Serial.println("ERROR: USB configuration command is too long");
            } else if (usbCommandLength > 0) {
                usbCommand[usbCommandLength] = '\0';
                configureBackendFromUsb(usbCommand);
            }
            usbCommandLength = 0;
            usbCommandOverflow = false;
            continue;
        }
        if (usbCommandLength < sizeof(usbCommand) - 1) {
            usbCommand[usbCommandLength++] = character;
        } else {
            usbCommandOverflow = true;
        }
    }
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println("Starting IoT Commander");

    board.init();
    if (!board.begin()) {
        haltWithError("Viewe display board initialization failed");
    }

    if (!lvglPortBegin(board.getLCD(), board.getTouch())) {
        haltWithError("LVGL display or touch initialization failed");
    }

    if (!settings.begin()) {
        Serial.println("ERROR: Local preferences could not be opened");
    }

    connectivity.begin();
    Serial.printf("[WiFi] Saved SSID at startup: '%s'\n",
                  settings.wifiSsid().c_str());
    if (!settings.wifiSsid().isEmpty()) {
        Serial.println("Attempting to reconnect to saved Wi-Fi");
        connectivity.connectWifi(settings.wifiSsid(), settings.wifiPassword());
    }
    mqtt.begin(notifications);
    backend.begin();
    backend.setConnection("wifi", "disconnected", "Waiting for a Wi-Fi connection.");
    backend.setConnection("mqtt", "disconnected", "Waiting for an MQTT connection.");
    notifications.push("Commander ready", "Scan for nearby systems to get started.");
    if (!ui.begin()) {
        haltWithError("User interface initialization failed");
    }
    Serial.println("Touchscreen terminal ready");
    Serial.println("USB provisioning ready");

    snapshotLock = xSemaphoreCreateMutex();
    refreshBackendSnapshot(connectivity.wifiConnected());
    xTaskCreatePinnedToCore(networkTask, "network", 20480, nullptr, 1, nullptr, 0);
}

void loop()
{
    updateUsbProvisioning();
    {
        SlowGuard guard("ui.update");
        ui.update();
    }
    const bool wifiConnected = connectivity.wifiConnected();
    if (wifiConnected != previousWifiConnected) {
        notifications.push(
            wifiConnected ? "Wi-Fi connected" : "Wi-Fi disconnected",
            wifiConnected ? connectivity.wifiSsid() : "The network connection was lost.");
        previousWifiConnected = wifiConnected;
        backend.setConnection(
            "wifi",
            wifiConnected ? "connected" : "disconnected",
            wifiConnected ? connectivity.wifiSsid() + " " + connectivity.ipAddress() : "Wi-Fi disconnected.");
    }
    const bool mqttConnected = mqtt.connected();
    if (mqttConnected != previousMqttConnected) {
        backend.setConnection(
            "mqtt",
            mqttConnected ? "connected" : "disconnected",
            mqttConnected ? settings.mqttSettings().host : "MQTT disconnected.");
        previousMqttConnected = mqttConnected;
    }
    static uint32_t lastSnapshotAt = 0;
    if (millis() - lastSnapshotAt >= 500) {
        lastSnapshotAt = millis();
        refreshBackendSnapshot(wifiConnected);
    }

    {
        SlowGuard guard("lvglPortHandle");
        lvglPortHandle();
    }
    delay(5);
}
