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

private:
    struct ConnectionEvent {
        String type;
        String state;
        String details;
    };

    bool post(const TerminalSettings& settings, const String& path, const String& body);
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
};
