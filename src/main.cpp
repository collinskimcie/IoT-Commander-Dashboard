#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <ArduinoJson.h>
#include <esp_display_panel.hpp>

#include "app/ConnectivityService.h"
#include "app/BackendService.h"
#include "app/MqttService.h"
#include "app/NotificationCenter.h"
#include "app/SettingsStore.h"
#include "app/ServerLocator.h"
#include "app/TimeService.h"
#include "app/SlowGuard.h"
#include "platform/LvglPort.h"
#include "platform/ScreenPower.h"
#include "ui/UiManager.h"

#include <cstring>
#include <array>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>

// Resolved at link time: null unless core dump to flash is compiled into the Arduino libraries.
extern "C" esp_err_t esp_core_dump_image_check(void) __attribute__((weak));

// Overrides the Arduino core default (which returns false and marks every image valid at startup).
// With true, a freshly updated image stays "pending verify" until BackendService confirms it,
// and the bootloader rolls back to the previous slot if it resets first. Needs C linkage.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

constexpr size_t kMaxUsbCommandLength = 3072;   // fits a 2048-byte PEM plus JSON escapes
constexpr uint32_t kTaskWdtTimeoutMs = 30000;     // TLS handshakes and timeouts can block for seconds
constexpr char kBackendConfigPrefix[] = "CONFIGURE_BACKEND ";
constexpr char kMqttConfigPrefix[] = "CONFIGURE_MQTT ";

esp_panel::board::Board board;
ConnectivityService connectivity;
NotificationCenter notifications;
SettingsStore settings;
ServerLocator serverLocator;
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

// Software watchdog: the network task stamps this every loop; loop() restarts the board if it stalls.
volatile uint32_t networkHeartbeatAt = 0;
constexpr uint32_t kNetworkStallLimitMs = 90000;
constexpr uint32_t kStallMarker = 0x57A11ED0;
RTC_NOINIT_ATTR uint32_t restartMarker;   // survives a software restart
bool bootedAfterStall = false;

const char* resetReasonName()
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return bootedAfterStall ? "net_stall" : "software";
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

// Copies the settings (two PEM roots plus the broker and backend strings, several KB) into
// the network snapshot only when they changed or Wi-Fi flipped, not on every call.
void refreshBackendSnapshot(bool wifiConnected)
{
    static bool haveCopy = false;
    static uint32_t copiedRevision = 0;
    static bool copiedWifi = false;
    const uint32_t settingsRevision = settings.revision();
    if (haveCopy && settingsRevision == copiedRevision && wifiConnected == copiedWifi) {
        return;
    }

    xSemaphoreTake(snapshotLock, portMAX_DELAY);
    if (!haveCopy || settingsRevision != copiedRevision) {
        networkSnapshot.terminal = settings.terminalSettings();
        networkSnapshot.mqtt = settings.mqttSettings();
    }
    networkSnapshot.wifiConnected = wifiConnected;
    ++networkSnapshot.revision;
    xSemaphoreGive(snapshotLock);

    haveCopy = true;
    copiedRevision = settingsRevision;
    copiedWifi = wifiConnected;
    log_i("net snapshot refreshed: settings rev=%u wifi=%s",
          static_cast<unsigned>(settingsRevision), wifiConnected ? "up" : "down");
}

// Called during a firmware download so a long transfer neither trips the watchdog nor the stall restart.
void otaTick()
{
    networkHeartbeatAt = millis() | 1U;
    esp_task_wdt_reset();
}

// One task owns all blocking network work (MQTT + backend), so LVGL never waits on it.
// A single task also avoids a second large stack, which internal RAM cannot spare.
void networkTask(void*)
{
    esp_task_wdt_add(nullptr);   // this task must keep looping or the watchdog panics
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
        backend.serviceFirmware(local.terminal, local.wifiConnected, otaTick);

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
        networkHeartbeatAt = millis() | 1U;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void configureTaskWatchdog()
{
    esp_task_wdt_config_t config = {};
    config.timeout_ms = kTaskWdtTimeoutMs;
    config.idle_core_mask = 1U;       // idle task of core 0 only, as the Arduino default
    config.trigger_panic = true;      // panic prints a backtrace and records reset=TASK_WDT
    if (esp_task_wdt_reconfigure(&config) != ESP_OK) {
        esp_task_wdt_init(&config);
    }
}

void reportCrashDiagnostics()
{
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (partition == nullptr) {
        Serial.println("coredump: no partition in the partition table");
        return;
    }
    if (esp_core_dump_image_check == nullptr) {
        Serial.printf("coredump: %u KB partition present, but core dump to flash is not compiled in\n",
            static_cast<unsigned>(partition->size / 1024));
        return;
    }
    Serial.printf("coredump: %u KB partition, stored dump: %s\n",
        static_cast<unsigned>(partition->size / 1024),
        esp_core_dump_image_check() == ESP_OK ? "YES" : "none");
}

void haltWithError(const char* message)
{
    Serial.printf("FATAL: %s\n", message);
    while (true) {
        delay(1000);
    }
}

// Copies a string field into `target` when the JSON has it; absent keeps the saved value.
bool takeUsbString(JsonDocument& document, const char* key, String& target)
{
    if (document[key].isNull()) {
        return true;
    }
    if (!document[key].is<const char*>()) {
        Serial.printf("ERROR: %s must be a string\n", key);
        return false;
    }
    target = document[key].as<const char*>();
    return true;
}

void configureMqttFromUsb(const char* json)
{
    JsonDocument document;
    if (deserializeJson(document, json) || !document.is<JsonObject>()) {
        Serial.println("ERROR: CONFIGURE_MQTT needs a JSON object");
        return;
    }
    MqttSettings updated = settings.mqttSettings();
    if (!takeUsbString(document, "host", updated.host) ||
        !takeUsbString(document, "username", updated.username) ||
        !takeUsbString(document, "password", updated.password) ||
        !takeUsbString(document, "rootCa", updated.rootCa) ||
        !takeUsbString(document, "telemetryTopic", updated.telemetryTopic) ||
        !takeUsbString(document, "commandTopic", updated.commandTopic)) {
        return;
    }
    if (!document["port"].isNull()) {
        const int port = document["port"].as<int>();
        if (!document["port"].is<int>() || port < 1 || port > 65535) {
            Serial.println("ERROR: port must be a number from 1 to 65535");
            return;
        }
        updated.port = static_cast<uint16_t>(port);
    }
    if (!settings.setMqttSettings(updated)) {
        Serial.println("ERROR: Could not save MQTT settings (a field is too long; the root CA limit is 2048 bytes)");
        return;
    }
    Serial.printf("USB MQTT settings saved: host=%s port=%u ca_bytes=%u\n",
        updated.host.c_str(), static_cast<unsigned>(updated.port),
        static_cast<unsigned>(updated.rootCa.length()));
}

void configureBackendFromUsb(const char* command)
{
    if (std::strcmp(command, "SELFTEST") == 0) {
        int passed = 0;
        int total = 0;
        const auto report = [&](const char* name, bool ok, const String& detail) {
            ++total;
            if (ok) {
                ++passed;
            }
            Serial.printf("SELFTEST %-8s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
        };
        report("wifi", connectivity.wifiConnected(),
            connectivity.wifiSsid() + " " + connectivity.ipAddress() + " rssi=" + String(WiFi.RSSI()));
        report("mqtt", mqtt.connected(), mqtt.status());
        report("backend", backendStatus() == BackendStatus::Ok, String(backendStatusName()));
        report("time", timeService.synchronized(), timeService.localTime());
        report("heap", ESP.getFreeHeap() > 60000 && ESP.getMaxAllocHeap() > 30000,
            "free=" + String(ESP.getFreeHeap()) + " maxblock=" + String(ESP.getMaxAllocHeap()));
        report("psram", ESP.getFreePsram() > 1000000, "free=" + String(ESP.getFreePsram()));
        Serial.printf("SELFTEST result: %d/%d passed\n", passed, total);
        return;
    }

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

    if (std::strncmp(command, kMqttConfigPrefix, sizeof(kMqttConfigPrefix) - 1) == 0) {
        configureMqttFromUsb(command + sizeof(kMqttConfigPrefix) - 1);
        return;
    }

    if (std::strncmp(command, kBackendConfigPrefix, sizeof(kBackendConfigPrefix) - 1) != 0) {
        Serial.printf("ERROR: Unknown USB command (%u bytes, first bytes:",
            static_cast<unsigned>(std::strlen(command)));
        for (size_t index = 0; index < 8 && command[index] != '\0'; ++index) {
            Serial.printf(" %02X", static_cast<unsigned>(static_cast<unsigned char>(command[index])));
        }
        Serial.println(")");
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
    if (!document["backendCa"].isNull()) {
        if (!document["backendCa"].is<const char*>()) {
            Serial.println("ERROR: backendCa must be a string");
            return;
        }
        updated.backendRootCa = document["backendCa"].as<const char*>();
    }
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
    Serial.setRxBufferSize(kMaxUsbCommandLength);   // a PEM line must fit between loop() polls
    Serial.begin(115200);
    delay(100);
    Serial.println("Starting IoT Commander");
    if (esp_reset_reason() == ESP_RST_SW && restartMarker == kStallMarker) {
        bootedAfterStall = true;
        Serial.println("Previous restart was caused by a network task stall");
    }
    restartMarker = 0;
    configureTaskWatchdog();
    reportCrashDiagnostics();

    board.init();
    if (!board.begin()) {
        haltWithError("Viewe display board initialization failed");
    }

    screenPowerBegin(board.getBacklight());
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
    esp_task_wdt_add(nullptr);   // the UI loop task is watched too
}

// Alerts raised by the backend arrive via the network task; show them from the main loop.
void deliverBackendAlerts()
{
    String message;
    uint8_t delivered = 0;
    while (delivered < 3 && backend.takeAlert(message)) {
        notifications.push("Backend alert", message);
        screenPowerWake();
        ++delivered;
    }
}

void loop()
{
    esp_task_wdt_reset();
    updateUsbProvisioning();
    screenPowerUpdate();
    deliverBackendAlerts();
    const uint32_t networkSeenAt = networkHeartbeatAt;
    if (networkSeenAt != 0 && static_cast<int32_t>(millis() - networkSeenAt) > static_cast<int32_t>(kNetworkStallLimitMs)) {
        Serial.printf("FATAL: network task stalled for %lu s; restarting\n",
            static_cast<unsigned long>((millis() - networkSeenAt) / 1000UL));
        Serial.flush();
        restartMarker = kStallMarker;
        delay(100);
        esp_restart();
    }
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

        static bool locatorStarted = false;
        if (!locatorStarted) {
            serverLocator.begin();
            locatorStarted = true;
        }
        serverLocator.update(settings.terminalSettings(), settings.mqttSettings(),
                             backendStatus() == BackendStatus::Ok);
        String newBackendHost;
        String newMqttHost;
        if (serverLocator.takeChange(newBackendHost, newMqttHost)) {
            TerminalSettings terminalUpdate = settings.terminalSettings();
            terminalUpdate.backendUrl = ServerLocator::withHost(terminalUpdate.backendUrl, newBackendHost);
            MqttSettings mqttUpdate = settings.mqttSettings();
            mqttUpdate.host = newMqttHost;
            const bool terminalSaved = settings.setTerminalSettings(terminalUpdate);
            const bool mqttSaved = settings.setMqttSettings(mqttUpdate);
            Serial.printf("[Server] Applied backend=%s mqtt=%s (saved: %s/%s)\n",
                newBackendHost.c_str(), newMqttHost.c_str(),
                terminalSaved ? "ok" : "FAIL", mqttSaved ? "ok" : "FAIL");
        }
    }

    {
        SlowGuard guard("lvglPortHandle");
        lvglPortHandle();
    }
    delay(5);
}
