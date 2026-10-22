#pragma once

#include "app/Domain.h"
#include "app/NotificationCenter.h"

#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include <array>
#include <atomic>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class MqttService {
public:
    void begin(NotificationCenter& notifications);

    // Network task only: connects, receives and publishes. May block for seconds.
    void service(const MqttSettings& settings);
    // UI thread only: hands queued notifications to the NotificationCenter. Never blocks.
    void pump();

    bool connected() const;
    String status() const;
    bool hasReading() const;
    float reading() const;
    std::array<TelemetryReading, kMaxTelemetryReadings> readings() const;
    // Network task only: it is the sole writer, so no lock or copy is needed.
    const std::array<TelemetryReading, kMaxTelemetryReadings>& readingsUnsafe() const;
    uint32_t readingRevision() const;
    // Queues the command; the network task publishes it. False if not connected.
    bool publishCommand(bool enabled);

private:
    struct PendingNote {
        String title;
        String message;
    };

    static void onMessage(char* topic, uint8_t* payload, unsigned int length);
    void serviceInternal(const MqttSettings& settings);
    void handleMessage(const char* topic, const uint8_t* payload, unsigned int length);
    void updateReading(const char* name, float value);
    bool settingsChanged(const MqttSettings& settings) const;
    void setStatus(const String& status);
    void notify(const String& title, const String& message);

    WiFiClientSecure _network;
    PubSubClient _client{_network};
    NotificationCenter* _notifications = nullptr;
    MqttSettings _settings;            // network task only
    String _status = "Configure MQTT in Settings";   // guarded by _lock
    String _clientId;
    String _statusTopic;   // retained online/offline marker (also the last-will topic)
    uint32_t _lastReconnectAttempt = 0;
    uint32_t _lastNotificationAt = 0;
    float _reading = 0.0F;             // guarded by _lock
    std::array<TelemetryReading, kMaxTelemetryReadings> _readings{};   // writes guarded by _lock
    uint32_t _readingRevision = 0;     // guarded by _lock
    bool _hasReading = false;          // guarded by _lock
    bool _hasThresholdState = false;
    bool _reportedConnection = false;
    bool _connectionAttempted = false;

    SemaphoreHandle_t _lock = nullptr;
    std::vector<PendingNote> _pendingNotes;   // guarded by _lock
    std::atomic<bool> _connected{false};
    std::atomic<bool> _hasCommandTopic{false};
    std::atomic<int8_t> _pendingCommand{-1};  // -1 none, 0 OFF, 1 ON
};
