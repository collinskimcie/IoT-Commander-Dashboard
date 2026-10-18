#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_display_panel.hpp>

#include "app/ConnectivityService.h"
#include "app/BackendService.h"
#include "app/MqttService.h"
#include "app/NotificationCenter.h"
#include "app/SettingsStore.h"
#include "app/TimeService.h"
#include "platform/LvglPort.h"
#include "ui/UiManager.h"

#include <cstring>

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
}

void loop()
{
    updateUsbProvisioning();
    ui.update();
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
    backend.update(
        settings.terminalSettings(),
        wifiConnected,
        mqtt.readings(),
        settings.mqttSettings().unit,
        mqtt.readingRevision());

    lvglPortHandle();
    delay(5);
}
