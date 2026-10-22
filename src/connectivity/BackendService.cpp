#include "app/BackendService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {

constexpr uint32_t kBaseIntervalMs = 5000;
constexpr uint32_t kMaxBackoffMs = 60000;
constexpr size_t kMaxPendingConnections = 8;

std::atomic<uint8_t> gStatus{static_cast<uint8_t>(BackendStatus::NotConfigured)};

void setStatus(BackendStatus status)
{
    gStatus.store(static_cast<uint8_t>(status));
}

class Guard {
public:
    explicit Guard(SemaphoreHandle_t handle) : _handle(handle)
    {
        if (_handle != nullptr) {
            xSemaphoreTake(_handle, portMAX_DELAY);
        }
    }
    ~Guard()
    {
        if (_handle != nullptr) {
            xSemaphoreGive(_handle);
        }
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    SemaphoreHandle_t _handle;
};

// A 4xx the server will never accept: retrying the same body cannot succeed.
// 401/403 (bad key), 408 (timeout) and 429 (rate limit) are transient or fixable.
bool permanentlyRejected(int code)
{
    return code >= 400 && code < 500 &&
        code != 401 && code != 403 && code != 408 && code != 429;
}

String apiPath(const String& base, const String& path)
{
    String result = base;
    while (result.endsWith("/")) {
        result.remove(result.length() - 1);
    }
    return result + path;
}

} // namespace

BackendStatus backendStatus()
{
    return static_cast<BackendStatus>(gStatus.load());
}

void BackendService::begin()
{
    _lock = xSemaphoreCreateMutex();
    char id[13];
    std::snprintf(id, sizeof(id), "%012llX", static_cast<unsigned long long>(ESP.getEfuseMac()));
    _terminalId = id;
}

void BackendService::setConnection(const char* type, const char* state, const String& details)
{
    if (type == nullptr || state == nullptr) {
        return;
    }
    Guard guard(_lock);
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

uint32_t BackendService::currentDelayMs() const
{
    if (_consecutiveFailures == 0) {
        return kBaseIntervalMs;
    }
    const uint32_t shift = std::min<uint32_t>(_consecutiveFailures, 5);
    return std::min<uint32_t>(kBaseIntervalMs << shift, kMaxBackoffMs);
}

void BackendService::noteResult(bool ok)
{
    if (ok) {
        if (backendStatus() != BackendStatus::Ok) {
            Serial.println("Backend connection OK");
        }
        _consecutiveFailures = 0;
        setStatus(BackendStatus::Ok);
        return;
    }
    if (_consecutiveFailures < 255) {
        ++_consecutiveFailures;
    }
    const bool authProblem = _lastHttpCode == 401 || _lastHttpCode == 403;
    setStatus(authProblem ? BackendStatus::AuthFailed : BackendStatus::Retrying);
    Serial.printf("Backend retry in %lu s\n", static_cast<unsigned long>(currentDelayMs() / 1000));
}

void BackendService::update(
    const TerminalSettings& settings,
    bool wifiConnected,
    const std::array<TelemetryReading, kMaxTelemetryReadings>& readings,
    const String& unit,
    uint32_t readingRevision)
{
    if (!validConfiguration(settings)) {
        setStatus(BackendStatus::NotConfigured);
        return;
    }
    if (!wifiConnected) {
        _wifiWasConnected = false;
        return;
    }

    const uint32_t now = millis();

    // New settings or a fresh Wi-Fi link: forget old failures and try right away.
    if (!_wifiWasConnected ||
        settings.backendUrl != _activeUrl ||
        settings.backendApiKey != _activeKey) {
        _wifiWasConnected = true;
        _activeUrl = settings.backendUrl;
        _activeKey = settings.backendApiKey;
        _consecutiveFailures = 0;
        _hasAttempted = false;
        _hasSentHeartbeat = false;
        setStatus(BackendStatus::Connecting);
    }

    if (_hasAttempted && now - _lastAttemptAt < currentDelayMs()) {
        return;
    }
    _lastAttemptAt = now;
    _hasAttempted = true;

    // Send the first heartbeat as soon as possible, then every 30 seconds.
    constexpr uint32_t kHeartbeatIntervalMs = 30000;
    if (!_hasSentHeartbeat || now - _lastHeartbeatAt >= kHeartbeatIntervalMs) {
        const bool ok = post(settings, "/api/v1/terminals/" + _terminalId + "/heartbeat", "{}");
        noteResult(ok);
        if (ok) {
            _lastHeartbeatAt = millis();
            _hasSentHeartbeat = true;
        }
        return;
    }

    // Drain queued connection transitions before routine reporting.
    ConnectionEvent event;
    bool hasEvent = false;
    {
        Guard guard(_lock);
        if (!_pendingConnections.empty()) {
            event = _pendingConnections.front();
            hasEvent = true;
        }
    }
    if (hasEvent) {
        JsonDocument document;
        document["type"] = event.type;
        document["state"] = event.state;
        document["details"] = event.details;
        String body;
        serializeJson(document, body);

        const bool ok = post(settings, "/api/v1/terminals/" + _terminalId + "/connections", body);
        const bool dropped = !ok && permanentlyRejected(_lastHttpCode);
        if (dropped) {
            Serial.printf("Backend rejected connection event (HTTP %d); dropping it\n", _lastHttpCode);
        }
        noteResult(ok || dropped);
        if (ok || dropped) {
            Guard guard(_lock);
            if (!_pendingConnections.empty()) {
                _pendingConnections.erase(_pendingConnections.begin());
            }
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
        now - _lastTelemetrySnapshotAt >= kTelemetrySnapshotIntervalMs;
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
    const bool ok = post(settings, "/api/v1/terminals/" + _terminalId + "/telemetry", body);
    const bool dropped = !ok && permanentlyRejected(_lastHttpCode);
    if (dropped) {
        Serial.printf("Backend rejected telemetry (HTTP %d); skipping this revision\n", _lastHttpCode);
    }
    noteResult(ok || dropped);
    if (ok || dropped) {
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
    http.setConnectTimeout(2500);
    http.setTimeout(3000);
    _lastHttpCode = 0;

    auto run = [&]() {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Api-Key", settings.backendApiKey);
        const int code = http.POST(body);
        http.end();
        return code;
    };

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
        responseCode = run();
    } else {
        WiFiClient client;
        client.setConnectionTimeout(1500);
        if (!http.begin(client, url)) {
            Serial.println("ERROR: Could not initialize backend request");
            return false;
        }
        responseCode = run();
    }

    _lastHttpCode = responseCode;
    if (responseCode < 200 || responseCode >= 300) {
        if (responseCode < 0) {
            Serial.printf(
                "ERROR: Backend request failed: %s (%d)\n",
                HTTPClient::errorToString(responseCode).c_str(),
                responseCode);
        } else {
            Serial.printf("ERROR: Backend request failed with HTTP status %d\n", responseCode);
        }
        return false;
    }
    return true;
}

bool BackendService::validConfiguration(const TerminalSettings& settings)
{
    if (settings.backendUrl.isEmpty() && settings.backendApiKey.isEmpty()) {
        _configErrorLogged = false;
        return false;
    }
    const bool supportedScheme =
        settings.backendUrl.startsWith("http://") || settings.backendUrl.startsWith("https://");
    if (!supportedScheme || settings.backendApiKey.isEmpty()) {
        if (!_configErrorLogged) {
            Serial.println("ERROR: Backend URL must use HTTP(S) and an API key must be configured");
            _configErrorLogged = true;
        }
        return false;
    }
    _configErrorLogged = false;
    return true;
}
