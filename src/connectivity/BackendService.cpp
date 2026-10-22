#include "app/BackendService.h"

#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>

namespace {

constexpr uint32_t kBaseIntervalMs = 5000;
constexpr uint32_t kMaxBackoffMs = 60000;
constexpr size_t kMaxPendingConnections = 8;
constexpr uint32_t kTelemetrySnapshotIntervalMs = 60000;
constexpr size_t kReadingQueueCapacity = 512;   // about 24 KB, allocated in PSRAM

// Store-and-forward: readings wait here until the backend has accepted them.
struct QueuedReading {
    char name[25];
    char unit[9];
    float value;
    uint32_t at;   // Unix seconds; 0 when the clock was not yet synchronized
};
QueuedReading* gReadingQueue = nullptr;
size_t gQueueHead = 0;    // index of the oldest entry
size_t gQueueCount = 0;
uint32_t gQueueDropped = 0;

bool queueReady()
{
    static bool allocationFailed = false;
    if (gReadingQueue == nullptr && !allocationFailed) {
        gReadingQueue = static_cast<QueuedReading*>(
            ps_malloc(sizeof(QueuedReading) * kReadingQueueCapacity));
        if (gReadingQueue == nullptr) {
            allocationFailed = true;
            Serial.println("ERROR: Could not allocate the telemetry queue in PSRAM");
        }
    }
    return gReadingQueue != nullptr;
}

void queuePush(const QueuedReading& item)
{
    if (!queueReady()) {
        return;
    }
    if (gQueueCount == kReadingQueueCapacity) {
        gQueueHead = (gQueueHead + 1) % kReadingQueueCapacity;
        --gQueueCount;
        if (++gQueueDropped == 1) {
            Serial.println("WARNING: telemetry queue full; dropping the oldest readings");
        }
    }
    gReadingQueue[(gQueueHead + gQueueCount) % kReadingQueueCapacity] = item;
    ++gQueueCount;
}

const QueuedReading& queueAt(size_t index)
{
    return gReadingQueue[(gQueueHead + index) % kReadingQueueCapacity];
}

void queuePop(size_t count)
{
    count = std::min(count, gQueueCount);
    gQueueHead = (gQueueHead + count) % kReadingQueueCapacity;
    gQueueCount -= count;
}
constexpr uint32_t kDrainIntervalMs = 400;   // gap between queued events while the link is healthy
constexpr bool kReuseConnection = true;      // keep-alive; false = one connection per request

// One long-lived HTTP client so the TCP/TLS session is reused between posts.
HTTPClient gHttp;
WiFiClient gPlain;
WiFiClientSecure gSecure;
String gConnectionBase;

void closeConnection()
{
    gHttp.end();
    gPlain.stop();
    gSecure.stop();
}

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

#ifndef FW_VERSION
#define FW_VERSION "0.3.0"
#endif

const char* resetReasonText()
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    default:               return "other";
    }
}

// Health snapshot sent with every heartbeat so the dashboard can show device health.
String heartbeatBody()
{
    JsonDocument document;
    document["fw"] = FW_VERSION;
    document["uptimeSeconds"] = static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
    document["heapFree"] = static_cast<uint32_t>(ESP.getFreeHeap());
    document["heapMaxBlock"] = static_cast<uint32_t>(ESP.getMaxAllocHeap());
    document["psramFree"] = static_cast<uint32_t>(ESP.getFreePsram());
    document["rssi"] = WiFi.RSSI();
    document["resetReason"] = resetReasonText();
    String body;
    serializeJson(document, body);
    return body;
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
    // Store readings as they change (and as a periodic snapshot), even while offline.
    {
        const uint32_t queuedAt = millis();
        bool hasValid = false;
        for (const TelemetryReading& reading : readings) {
            if (reading.valid) {
                hasValid = true;
                break;
            }
        }
        const bool changed = readingRevision != 0 &&
            (!_hasSentReading || readingRevision != _lastReadingRevision);
        const bool snapshotDue = !_hasSentTelemetrySnapshot ||
            queuedAt - _lastTelemetrySnapshotAt >= kTelemetrySnapshotIntervalMs;
        if (hasValid && (changed || snapshotDue)) {
            const time_t wall = time(nullptr);
            const uint32_t stamp = wall > 1700000000 ? static_cast<uint32_t>(wall) : 0;
            for (const TelemetryReading& reading : readings) {
                if (!reading.valid) {
                    continue;
                }
                QueuedReading item = {};
                std::snprintf(item.name, sizeof(item.name), "%s", reading.name.c_str());
                std::snprintf(item.unit, sizeof(item.unit), "%s", unit.c_str());
                item.value = reading.value;
                item.at = stamp;
                queuePush(item);
            }
            _lastReadingRevision = readingRevision;
            _hasSentReading = true;
            _lastTelemetrySnapshotAt = queuedAt;
            _hasSentTelemetrySnapshot = true;
        }
    }

    if (!validConfiguration(settings)) {
        setStatus(BackendStatus::NotConfigured);
        return;
    }
    if (!wifiConnected) {
        if (_wifiWasConnected) {
            closeConnection();
        }
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

    // Drain queued connection events quickly while the link is healthy; otherwise honour the backoff.
    size_t queued = 0;
    {
        Guard guard(_lock);
        queued = _pendingConnections.size();
    }
    const uint32_t gateMs =
        (_consecutiveFailures == 0 && (queued > 0 || gQueueCount > 0)) ? kDrainIntervalMs : currentDelayMs();
    if (_hasAttempted && now - _lastAttemptAt < gateMs) {
        return;
    }
    _lastAttemptAt = now;
    _hasAttempted = true;

    // Send the first heartbeat as soon as possible, then every 30 seconds.
    constexpr uint32_t kHeartbeatIntervalMs = 30000;
    if (!_hasSentHeartbeat || now - _lastHeartbeatAt >= kHeartbeatIntervalMs) {
        const bool ok = post(settings, "/api/v1/terminals/" + _terminalId + "/heartbeat", heartbeatBody());
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

    // Drain stored readings oldest first; remove them only once the backend has them.
    if (gQueueCount == 0) {
        return;
    }
    const size_t batch = std::min<size_t>(gQueueCount, kMaxTelemetryReadings);
    JsonDocument document;
    JsonArray values = document["readings"].to<JsonArray>();
    for (size_t index = 0; index < batch; ++index) {
        const QueuedReading& item = queueAt(index);
        JsonObject entry = values.add<JsonObject>();
        entry["name"] = item.name;
        entry["value"] = item.value;
        entry["unit"] = item.unit;
        if (item.at != 0) {
            entry["observedAt"] = item.at;
        }
    }
    String body;
    serializeJson(document, body);

    const bool ok = post(settings, "/api/v1/terminals/" + _terminalId + "/telemetry", body);
    const bool dropped = !ok && permanentlyRejected(_lastHttpCode);
    if (dropped) {
        Serial.printf("Backend rejected telemetry (HTTP %d); dropping %u readings\n",
            _lastHttpCode, static_cast<unsigned>(batch));
    }
    noteResult(ok || dropped);
    if (ok || dropped) {
        queuePop(batch);
        if (ok && gQueueCount > 0) {
            Serial.printf("Backend: %u stored readings left to send\n", static_cast<unsigned>(gQueueCount));
        }
    }
}

bool BackendService::post(const TerminalSettings& settings, const String& path, const String& body)
{
    const String url = apiPath(settings.backendUrl, path);
    const bool secure = settings.backendUrl.startsWith("https://");
    _lastHttpCode = 0;

    if (secure && settings.backendRootCa.isEmpty()) {
        Serial.println("ERROR: HTTPS backend requires a configured root CA certificate");
        return false;
    }

    // A different backend cannot reuse the open connection.
    if (settings.backendUrl != gConnectionBase) {
        closeConnection();
        gConnectionBase = settings.backendUrl;
    }

    auto attempt = [&](WiFiClient& client) -> int {
        if (secure) {
            // Re-point at the current CA string: the settings snapshot may have been reallocated.
            gSecure.setCACert(settings.backendRootCa.c_str());
            gSecure.setHandshakeTimeout(3);
        }
        client.setConnectionTimeout(1500);
        gHttp.setReuse(kReuseConnection);
        gHttp.setConnectTimeout(2500);
        gHttp.setTimeout(3000);
        if (!gHttp.begin(client, url)) {
            Serial.println("ERROR: Could not initialize backend request");
            return HTTPC_ERROR_CONNECTION_REFUSED;
        }
        gHttp.addHeader("Content-Type", "application/json");
        gHttp.addHeader("X-Api-Key", settings.backendApiKey);
        const int code = gHttp.POST(body);
        if (code > 0) {
            (void)gHttp.getString();   // read the reply so the connection can be reused
        }
        gHttp.end();
        return code;
    };

    const bool wasOpen = secure ? gSecure.connected() : gPlain.connected();
    int responseCode = secure ? attempt(gSecure) : attempt(gPlain);
    if (responseCode < 0 && wasOpen) {
        // The server may have closed an idle keep-alive connection: retry once on a fresh one.
        closeConnection();
        responseCode = secure ? attempt(gSecure) : attempt(gPlain);
    }
    if (responseCode < 0 || !kReuseConnection) {
        closeConnection();
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
