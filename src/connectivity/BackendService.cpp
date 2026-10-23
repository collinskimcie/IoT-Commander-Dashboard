#include "app/BackendService.h"

#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <cstdlib>

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
#define FW_VERSION "0.5.1"
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

    pollAlerts(settings, wifiConnected);

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

namespace {

constexpr uint32_t kAlertPollIntervalMs = 15000;
constexpr size_t kAlertSlots = 8;          // alerts waiting for the main loop
constexpr size_t kAlertTextLength = 161;
char gAlertText[kAlertSlots][kAlertTextLength];
volatile uint8_t gAlertHead = 0;           // next slot the main loop reads
volatile uint8_t gAlertTail = 0;           // next slot the network task writes

void pushAlertText(const char* text)
{
    const uint8_t tail = gAlertTail;
    const uint8_t next = static_cast<uint8_t>((tail + 1) % kAlertSlots);
    if (next == gAlertHead) {
        Serial.println("WARNING: alert queue full; dropping an alert");
        return;
    }
    std::snprintf(gAlertText[tail], kAlertTextLength, "%s", text);
    __sync_synchronize();
    gAlertTail = next;
}

} // namespace

bool BackendService::takeAlert(String& message)
{
    const uint8_t head = gAlertHead;
    if (head == gAlertTail) {
        return false;
    }
    __sync_synchronize();
    message = gAlertText[head];
    __sync_synchronize();
    gAlertHead = static_cast<uint8_t>((head + 1) % kAlertSlots);
    return true;
}

bool BackendService::get(const TerminalSettings& settings, const String& path, String& response)
{
    const String url = apiPath(settings.backendUrl, path);
    const bool secure = settings.backendUrl.startsWith("https://");
    _lastHttpCode = 0;
    response = "";

    if (secure && settings.backendRootCa.isEmpty()) {
        return false;
    }

    // A different backend cannot reuse the open connection.
    if (settings.backendUrl != gConnectionBase) {
        closeConnection();
        gConnectionBase = settings.backendUrl;
    }

    auto attempt = [&](WiFiClient& client) -> int {
        if (secure) {
            gSecure.setCACert(settings.backendRootCa.c_str());
            gSecure.setHandshakeTimeout(3);
        }
        client.setConnectionTimeout(1500);
        gHttp.setReuse(kReuseConnection);
        gHttp.setConnectTimeout(2500);
        gHttp.setTimeout(3000);
        if (!gHttp.begin(client, url)) {
            return HTTPC_ERROR_CONNECTION_REFUSED;
        }
        gHttp.addHeader("X-Api-Key", settings.backendApiKey);
        const int code = gHttp.GET();
        if (code > 0) {
            response = gHttp.getString();
        }
        gHttp.end();
        return code;
    };

    const bool wasOpen = secure ? gSecure.connected() : gPlain.connected();
    int responseCode = secure ? attempt(gSecure) : attempt(gPlain);
    if (responseCode < 0 && wasOpen) {
        closeConnection();
        responseCode = secure ? attempt(gSecure) : attempt(gPlain);
    }
    if (responseCode < 0 || !kReuseConnection) {
        closeConnection();
    }

    _lastHttpCode = responseCode;
    if (responseCode < 200 || responseCode >= 300) {
        Serial.printf("WARNING: Alert poll failed (HTTP %d)\n", responseCode);
        return false;
    }
    return true;
}

void BackendService::pollAlerts(const TerminalSettings& settings, bool wifiConnected)
{
    if (!wifiConnected || _consecutiveFailures > 0 || _terminalId.isEmpty() || settings.backendUrl.isEmpty()) {
        return;
    }
    const uint32_t nowMs = millis();
    if (_alertsPolledOnce && nowMs - _lastAlertPollAt < kAlertPollIntervalMs) {
        return;
    }
    _alertsPolledOnce = true;
    _lastAlertPollAt = nowMs;

    String path = "/api/v1/terminals/" + _terminalId + "/alerts";
    if (_alertsBootstrapped) {
        path += "?afterId=" + String(_lastAlertId) + "&limit=5";
    }

    String reply;
    if (!get(settings, path, reply)) {
        return;
    }

    JsonDocument document;
    if (deserializeJson(document, reply)) {
        Serial.println("WARNING: Alert poll returned invalid JSON");
        return;
    }

    const uint32_t latestId = document["latestId"] | 0U;
    if (!_alertsBootstrapped || latestId < _lastAlertId) {
        // First poll after boot (or the backend counter restarted): start from now, never replay history.
        _lastAlertId = latestId;
        _alertsBootstrapped = true;
        Serial.printf("Backend alerts: following from id %lu\n", static_cast<unsigned long>(latestId));
        return;
    }

    for (JsonObject alert : document["alerts"].as<JsonArray>()) {
        const uint32_t id = alert["id"] | 0U;
        const char* message = alert["message"] | "";
        if (id <= _lastAlertId) {
            continue;
        }
        _lastAlertId = id;
        if (message[0] == '\0') {
            continue;
        }
        Serial.printf("Backend alert %lu: %s\n", static_cast<unsigned long>(id), message);
        pushAlertText(message);
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

// ---------------------------------------------------------------------------
// OTA firmware update. The dashboard requests an image; the terminal pulls it.
// Flow: poll offer -> download into the idle app slot -> verify SHA-256 -> activate -> restart.
// verifyRollbackLater() in main.cpp keeps the new image "pending verify" until the backend answers,
// otherwise the bootloader (or the 2-minute deadline below) rolls back to the previous slot.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kFirmwareFirstPollMs = 20000;
constexpr uint32_t kFirmwarePollIntervalMs = 60000;
constexpr uint32_t kReportRetryMs = 10000;
constexpr uint32_t kVerifyDeadlineMs = 120000;   // a new image must reach the backend within this time
constexpr size_t kOtaChunkBytes = 2048;

bool gFirmwareBootChecked = false;
bool gPendingVerify = false;
uint32_t gFirmwarePolledAt = 0;
uint32_t gReportAttemptAt = 0;
uint8_t gReportKind = 0;          // 0 none, 1 update ok, 2 update failed
uint32_t gReportImageId = 0;
String gReportDetail;

void saveReport(uint32_t imageId, uint8_t kind, const String& detail)
{
    gReportKind = kind;
    gReportImageId = imageId;
    gReportDetail = detail.substring(0, 100);
    Preferences prefs;
    if (prefs.begin("ota", false)) {
        prefs.putUInt("rid", imageId);
        prefs.putString("rmsg", gReportDetail);
        prefs.putUChar("report", kind);
        prefs.putUInt("tried", 0);
        prefs.end();
    }
}

void clearReport()
{
    gReportKind = 0;
    Preferences prefs;
    if (prefs.begin("ota", false)) {
        prefs.putUChar("report", 0);
        prefs.end();
    }
}

void firmwareBootCheck()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    gPendingVerify = running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;

    uint32_t tried = 0;
    Preferences prefs;
    if (prefs.begin("ota", false)) {
        gReportKind = prefs.getUChar("report", 0);
        gReportImageId = prefs.getUInt("rid", 0);
        gReportDetail = prefs.getString("rmsg", "");
        tried = prefs.getUInt("tried", 0);
        prefs.end();
    }
    Serial.printf("OTA: running %s, pending verify: %s\n", FW_VERSION, gPendingVerify ? "yes" : "no");
    if (!gPendingVerify && tried != 0 && gReportKind == 0) {
        // An update was started, but this is not the new image: it was interrupted or rolled back.
        saveReport(tried, 2, "update did not run; interrupted or rolled back");
    }
}

String toHex(const uint8_t* data, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    String out;
    out.reserve(length * 2);
    for (size_t index = 0; index < length; ++index) {
        out += digits[data[index] >> 4];
        out += digits[data[index] & 0x0F];
    }
    return out;
}

} // namespace

bool BackendService::installFirmware(
    const TerminalSettings& settings,
    uint32_t imageId,
    uint32_t size,
    const String& sha256,
    String& error,
    void (*tick)())
{
    const bool secure = settings.backendUrl.startsWith("https://");
    if (secure && settings.backendRootCa.isEmpty()) {
        error = "HTTPS needs a root CA";
        return false;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr || size > target->size) {
        error = "image does not fit the idle app slot";
        return false;
    }
    uint8_t* buffer = static_cast<uint8_t*>(malloc(kOtaChunkBytes));
    if (buffer == nullptr) {
        error = "out of memory";
        return false;
    }

    bool begun = false;
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts(&hash, 0);

    auto fail = [&](const String& reason) {
        error = reason;
        if (begun) {
            Update.abort();
        }
        gHttp.end();
        closeConnection();
        mbedtls_sha256_free(&hash);
        free(buffer);
        buffer = nullptr;
        return false;
    };

    closeConnection();   // a fresh connection, not the keep-alive one
    gConnectionBase = settings.backendUrl;
    WiFiClient& client = secure ? static_cast<WiFiClient&>(gSecure) : gPlain;
    if (secure) {
        gSecure.setCACert(settings.backendRootCa.c_str());
        gSecure.setHandshakeTimeout(5);
    }
    client.setConnectionTimeout(5000);
    gHttp.setReuse(false);
    gHttp.setConnectTimeout(5000);
    gHttp.setTimeout(15000);

    const String url = apiPath(settings.backendUrl, "/api/v1/firmware/" + String(imageId) + "/download");
    if (!gHttp.begin(client, url)) {
        return fail("could not start the download");
    }
    gHttp.addHeader("X-Api-Key", settings.backendApiKey);
    const int code = gHttp.GET();
    if (code != HTTP_CODE_OK) {
        return fail("download failed (HTTP " + String(code) + ")");
    }
    if (gHttp.getSize() != static_cast<int>(size)) {
        return fail("download size differs from the offer");
    }
    if (!Update.begin(size, U_FLASH)) {
        return fail(String("Update.begin: ") + Update.errorString());
    }
    begun = true;

    WiFiClient* stream = gHttp.getStreamPtr();
    uint32_t remaining = size;
    uint32_t lastDataAt = millis();
    uint32_t lastLogAt = lastDataAt;
    while (remaining > 0) {
        const size_t available = stream->available();
        if (available == 0) {
            if (millis() - lastDataAt > 15000) {
                return fail("download stalled");
            }
            if (!stream->connected()) {
                return fail("connection closed early");
            }
            if (tick != nullptr) {
                tick();
            }
            delay(5);
            continue;
        }
        const size_t want = std::min<size_t>(std::min<size_t>(available, kOtaChunkBytes), remaining);
        const int got = stream->readBytes(buffer, want);
        if (got <= 0) {
            continue;
        }
        if (Update.write(buffer, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
            return fail(String("flash write: ") + Update.errorString());
        }
        mbedtls_sha256_update(&hash, buffer, static_cast<size_t>(got));
        remaining -= static_cast<uint32_t>(got);
        lastDataAt = millis();
        if (tick != nullptr) {
            tick();
        }
        if (millis() - lastLogAt > 5000) {
            lastLogAt = millis();
            Serial.printf("OTA: %u%%\n", static_cast<unsigned>((size - remaining) * 100ULL / size));
        }
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&hash, digest);
    mbedtls_sha256_free(&hash);
    if (!toHex(digest, sizeof(digest)).equalsIgnoreCase(sha256)) {
        return fail("SHA-256 does not match the offer");
    }
    gHttp.end();
    closeConnection();
    free(buffer);
    buffer = nullptr;
    // end(true) validates the image and only then makes the idle slot the boot slot.
    if (!Update.end(true)) {
        return fail(String("Update.end: ") + Update.errorString());
    }
    return true;
}

void BackendService::serviceFirmware(const TerminalSettings& settings, bool wifiConnected, void (*tick)())
{
    if (!gFirmwareBootChecked) {
        gFirmwareBootChecked = true;
        firmwareBootCheck();
    }
    const uint32_t now = millis();

    if (gPendingVerify) {
        if (backendStatus() == BackendStatus::Ok) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                gPendingVerify = false;
                Serial.println("OTA: new firmware confirmed; rollback cancelled");
                uint32_t tried = 0;
                Preferences prefs;
                if (prefs.begin("ota", true)) {
                    tried = prefs.getUInt("tried", 0);
                    prefs.end();
                }
                if (tried != 0) {
                    saveReport(tried, 1, String("running ") + FW_VERSION);
                }
            }
        } else if (now > kVerifyDeadlineMs) {
            Serial.println("OTA: new firmware could not reach the backend; rolling back");
            Serial.flush();
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        return;
    }

    if (!wifiConnected || _terminalId.isEmpty() || backendStatus() != BackendStatus::Ok) {
        return;
    }

    if (gReportKind != 0) {
        if (gReportAttemptAt != 0 && now - gReportAttemptAt < kReportRetryMs) {
            return;
        }
        gReportAttemptAt = now;
        JsonDocument document;
        document["imageId"] = gReportImageId;
        document["ok"] = gReportKind == 1;
        document["detail"] = gReportDetail;
        String body;
        serializeJson(document, body);
        const bool ok = post(settings, "/api/v1/terminals/" + _terminalId + "/firmware/result", body);
        if (ok || permanentlyRejected(_lastHttpCode)) {
            clearReport();
        }
        return;
    }

    if (gFirmwarePolledAt != 0 ? now - gFirmwarePolledAt < kFirmwarePollIntervalMs : now < kFirmwareFirstPollMs) {
        return;
    }
    gFirmwarePolledAt = now;

    String reply;
    if (!get(settings, "/api/v1/terminals/" + _terminalId + "/firmware", reply)) {
        return;
    }
    JsonDocument document;
    if (deserializeJson(document, reply) || !(document["available"] | false)) {
        return;
    }
    const uint32_t imageId = document["imageId"] | 0U;
    const uint32_t size = document["size"] | 0U;
    const String version = document["version"] | "";
    const String sha256 = document["sha256"] | "";
    if (imageId == 0 || size == 0 || sha256.length() != 64) {
        return;
    }
    if (version == FW_VERSION) {
        saveReport(imageId, 2, "already running " + version);
        return;
    }

    Serial.printf("OTA: installing %s (%u bytes)\n", version.c_str(), static_cast<unsigned>(size));
    {
        Preferences prefs;
        if (prefs.begin("ota", false)) {
            prefs.putUInt("tried", imageId);
            prefs.end();
        }
    }
    String error;
    if (!installFirmware(settings, imageId, size, sha256, error, tick)) {
        Serial.printf("OTA: failed: %s\n", error.c_str());
        saveReport(imageId, 2, error);
        return;
    }
    Serial.println("OTA: image verified; restarting into it");
    Serial.flush();
    delay(200);
    esp_restart();
}
