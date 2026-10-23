#pragma once

#include "app/Domain.h"

#include <array>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class BackendStatus : uint8_t {
    NotConfigured,
    Connecting,
    Ok,
    Retrying,
    AuthFailed,
};

// Thread-safe; can be read from the UI at any time.
BackendStatus backendStatus();

class BackendService {
public:
    void begin();
    void setConnection(const char* type, const char* state, const String& details);
    void update(
        const TerminalSettings& settings,
        bool wifiConnected,
        const std::array<TelemetryReading, kMaxTelemetryReadings>& readings,
        const String& unit,
        uint32_t readingRevision);

    // The network task queues alerts; the main loop takes them (lock-free, one writer and one reader).
    bool takeAlert(String& message);

    // Network task: applies a firmware update the dashboard asked for (see the OTA notes in BackendService.cpp).
    // `tick` is called during the download so the watchdogs stay fed.
    void serviceFirmware(const TerminalSettings& settings, bool wifiConnected, void (*tick)());

private:
    struct ConnectionEvent {
        String type;
        String state;
        String details;
    };

    bool post(const TerminalSettings& settings, const String& path, const String& body);
    bool get(const TerminalSettings& settings, const String& path, String& response);
    void pollAlerts(const TerminalSettings& settings, bool wifiConnected);
    bool installFirmware(const TerminalSettings& settings, uint32_t imageId, uint32_t size,
                         const String& sha256, String& error, void (*tick)());
    bool validConfiguration(const TerminalSettings& settings);
    void noteResult(bool ok);
    uint32_t currentDelayMs() const;

    SemaphoreHandle_t _lock = nullptr;
    std::vector<ConnectionEvent> _pendingConnections;
    String _terminalId;
    String _activeUrl;
    String _activeKey;
    uint32_t _lastAttemptAt = 0;
    uint32_t _lastReadingRevision = 0;
    uint32_t _lastHeartbeatAt = 0;
    uint32_t _lastTelemetrySnapshotAt = 0;
    int _lastHttpCode = 0;
    uint8_t _consecutiveFailures = 0;
    bool _hasAttempted = false;
    bool _wifiWasConnected = false;
    bool _configErrorLogged = false;
    bool _hasSentReading = false;
    bool _hasSentHeartbeat = false;
    bool _hasSentTelemetrySnapshot = false;
    uint32_t _lastAlertPollAt = 0;
    uint32_t _lastAlertId = 0;
    bool _alertsPolledOnce = false;
    bool _alertsBootstrapped = false;
};
