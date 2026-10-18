#pragma once

#include <Arduino.h>
#include <array>
#include <vector>

constexpr uint8_t kMaxTelemetryReadings = 4;

enum class Transport : uint8_t {
    Wifi,
    Bluetooth
};

struct NearbyDevice {
    String id;
    String name;
    Transport transport;
    int rssi;
    bool secured;
};

struct SavedDevice {
    String id;
    String name;
    Transport transport;
};

struct MqttSettings {
    String host;
    uint16_t port = 1883;
    String username;
    String password;
    String rootCa;
    String telemetryTopic;
    String commandTopic;
    String tileLabel = "Sensor";
    String unit;
    String visibleFields;
    float alertThreshold = 0.0F;
    bool alertEnabled = false;
    bool tileEnabled = true;
};

struct TerminalSettings {
    String backendUrl;
    String backendApiKey;
    String backendRootCa;
    String timezone = "UTC-3";
};

struct TelemetryReading {
    String name;
    float value = 0.0F;
    uint32_t updatedAt = 0;
    bool valid = false;
};

struct Notification {
    uint32_t id;
    String title;
    String message;
    uint32_t createdAt;
    bool unread;
};
