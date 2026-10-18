#pragma once

#include "app/Domain.h"
#include "app/NotificationCenter.h"

#include <PubSubClient.h>
#include <WiFiClientSecure.h>

class MqttService {
public:
    void begin(NotificationCenter& notifications);
    void update(const MqttSettings& settings);

    bool connected();
    String status() const;
    bool hasReading() const;
    float reading() const;
    const std::array<TelemetryReading, kMaxTelemetryReadings>& readings() const;
    uint32_t readingRevision() const;
    bool publishCommand(bool enabled);

private:
    static void onMessage(char* topic, uint8_t* payload, unsigned int length);
    void handleMessage(const char* topic, const uint8_t* payload, unsigned int length);
    void updateReading(const char* name, float value);
    bool settingsChanged(const MqttSettings& settings) const;
    void setStatus(const String& status);

    WiFiClientSecure _network;
    PubSubClient _client{_network};
    NotificationCenter* _notifications = nullptr;
    MqttSettings _settings;
    String _status = "Configure MQTT in Settings";
    String _clientId;
    uint32_t _lastReconnectAttempt = 0;
    uint32_t _lastNotificationAt = 0;
    float _reading = 0.0F;
    std::array<TelemetryReading, kMaxTelemetryReadings> _readings{};
    uint32_t _readingRevision = 0;
    bool _hasReading = false;
    bool _wasAboveThreshold = false;
    bool _hasThresholdState = false;
    bool _reportedConnection = false;
    bool _connectionAttempted = false;
};
