#include "app/BackendService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {

constexpr uint32_t kRetryIntervalMs = 5000;
constexpr size_t kMaxPendingConnections = 8;

String apiPath(const String& base, const String& path)
{
    String result = base;
    while (result.endsWith("/")) {
        result.remove(result.length() - 1);
    }
    return result + path;
}

} // namespace

void BackendService::begin()
{
    char id[13];
    std::snprintf(id, sizeof(id), "%012llX", static_cast<unsigned long long>(ESP.getEfuseMac()));
    _terminalId = id;
}

void BackendService::setConnection(const char* type, const char* state, const String& details)
{
    if (type == nullptr || state == nullptr) {
        return;
    }
    if (!_pendingConnections.empty() &&
        _pendingConnections.back().type == type &&
        _pendingConnections.back().state == state &&
        _pendingConnections.back().details == details) {
        return;
    }
    if (_pendingConnections.size() >= kMaxPendingConnections) {
        Serial.println("ERROR: Backend connection-event queue is full; event was not queued");
        return;
    }
    _pendingConnections.push_back({type, state, details});
}

void BackendService::update(
    const TerminalSettings& settings,
    bool wifiConnected,
    const std::array<TelemetryReading, kMaxTelemetryReadings>& readings,
    const String& unit,
    uint32_t readingRevision)
{
    if (!validConfiguration(settings) || !wifiConnected ||
        millis() - _lastAttemptAt < kRetryIntervalMs) {
        return;
    }

    _lastAttemptAt = millis();

    // Send the first heartbeat as soon as possible, then every 30 seconds.
    // This does not depend on sensor values changing.
    constexpr uint32_t kHeartbeatIntervalMs = 30000;
    if (!_hasSentHeartbeat ||
        millis() - _lastHeartbeatAt >= kHeartbeatIntervalMs) {
        if (post(
                settings,
                "/api/v1/terminals/" + _terminalId + "/heartbeat",
                "{}")) {
            _lastHeartbeatAt = millis();
            _hasSentHeartbeat = true;
        }
        return;
    }

    // Drain queued connection transitions before routine reporting.
    if (!_pendingConnections.empty()) {
        const ConnectionEvent& event = _pendingConnections.front();
        JsonDocument document;
        document["type"] = event.type;
        document["state"] = event.state;
        document["details"] = event.details;
        String body;
        serializeJson(document, body);

        if (post(
                settings,
                "/api/v1/terminals/" + _terminalId + "/connections",
                body)) {
            _pendingConnections.erase(_pendingConnections.begin());
        }
        return;
    }

    // Send changed readings promptly and periodically snapshot unchanged values.
    constexpr uint32_t kTelemetrySnapshotIntervalMs = 60000;

    bool hasValidReading = false;
    for (const TelemetryReading& reading : readings) {
        if (reading.valid) {
            hasValidReading = true;
            break;
        }
    }

    if (!hasValidReading) {
        return;
    }

    const bool readingChanged =
        readingRevision != 0 &&
        (!_hasSentReading || readingRevision != _lastReadingRevision);
    const bool snapshotDue =
        !_hasSentTelemetrySnapshot ||
        millis() - _lastTelemetrySnapshotAt >= kTelemetrySnapshotIntervalMs;

    if (!readingChanged && !snapshotDue) {
        return;
    }

    JsonDocument document;
    JsonArray values = document["readings"].to<JsonArray>();
    for (const TelemetryReading& reading : readings) {
        if (!reading.valid) {
            continue;
        }
        JsonObject item = values.add<JsonObject>();
        item["name"] = reading.name;
        item["value"] = reading.value;
        item["unit"] = unit;
    }

    if (values.size() == 0) {
        return;
    }

    String body;
    serializeJson(document, body);
    if (post(settings, "/api/v1/terminals/" + _terminalId + "/telemetry", body)) {
        _lastReadingRevision = readingRevision;
        _hasSentReading = true;
        _lastTelemetrySnapshotAt = millis();
        _hasSentTelemetrySnapshot = true;
    }
}

bool BackendService::post(const TerminalSettings& settings, const String& path, const String& body)
{
    HTTPClient http;
    const String url = apiPath(settings.backendUrl, path);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key", settings.backendApiKey);

    int responseCode = 0;
    if (settings.backendUrl.startsWith("https://")) {
        if (settings.backendRootCa.isEmpty()) {
            Serial.println("ERROR: HTTPS backend requires a configured root CA certificate");
            return false;
        }
        WiFiClientSecure client;
        client.setConnectionTimeout(1500);
        client.setHandshakeTimeout(3);
        client.setCACert(settings.backendRootCa.c_str());
        if (!http.begin(client, url)) {
            Serial.println("ERROR: Could not initialize HTTPS backend request");
            return false;
        }
        responseCode = http.POST(body);
        http.end();
    } else {
        WiFiClient client;
        client.setConnectionTimeout(1500);
        if (!http.begin(client, url)) {
            Serial.println("ERROR: Could not initialize backend request");
            return false;
        }
        responseCode = http.POST(body);
        http.end();
    }

    if (responseCode < 200 || responseCode >= 300) {
        Serial.printf("ERROR: Backend request failed with HTTP status %d\n", responseCode);
        return false;
    }
    return true;
}

bool BackendService::validConfiguration(const TerminalSettings& settings) const
{
    const bool supportedScheme =
        settings.backendUrl.startsWith("http://") || settings.backendUrl.startsWith("https://");
    if (settings.backendUrl.isEmpty() && settings.backendApiKey.isEmpty()) {
        return false;
    }
    if (!supportedScheme || settings.backendApiKey.isEmpty()) {
        Serial.println("ERROR: Backend URL must use HTTP(S) and an API key must be configured");
        return false;
    }
    return true;
}
