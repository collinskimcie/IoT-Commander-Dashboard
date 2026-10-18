#pragma once

#include "app/Domain.h"

#include <vector>

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
    bool validConfiguration(const TerminalSettings& settings) const;
    String terminalId() const;

    std::vector<ConnectionEvent> _pendingConnections;
    String _terminalId;
    uint32_t _lastAttemptAt = 0;
    uint32_t _lastReadingRevision = 0;
    bool _hasSentReading = false;
};
