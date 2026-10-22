#include "app/MqttService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <esp_system.h>
#include <WiFi.h>

namespace {

constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr uint32_t kAlertRepeatIntervalMs = 60000;
constexpr size_t kMaxPayloadLength = 384;
constexpr size_t kMaxPendingNotes = 8;
MqttService* activeService = nullptr;

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

bool topicMatches(const String& filter, const char* topic)
{
    if (topic == nullptr) {
        return false;
    }

    size_t filterStart = 0;
    size_t topicStart = 0;
    while (filterStart < filter.length()) {
        const int filterEnd = filter.indexOf('/', filterStart);
        const int topicEnd = std::strchr(topic + topicStart, '/') == nullptr
            ? -1
            : static_cast<int>(std::strchr(topic + topicStart, '/') - topic);
        const size_t filterLength = (filterEnd < 0 ? filter.length() : filterEnd) - filterStart;
        const size_t topicLength = (topicEnd < 0 ? std::strlen(topic) : topicEnd) - topicStart;
        const String filterPart = filter.substring(filterStart, filterStart + filterLength);

        if (filterPart == "#") {
            return filterEnd < 0;
        }
        if (filterPart != "+" &&
            (filterLength != topicLength || std::strncmp(filter.c_str() + filterStart, topic + topicStart, filterLength) != 0)) {
            return false;
        }

        if (filterEnd < 0 || topicEnd < 0) {
            return filterEnd < 0 && topicEnd < 0;
        }
        filterStart = static_cast<size_t>(filterEnd) + 1;
        topicStart = static_cast<size_t>(topicEnd) + 1;
    }
    return topicStart == std::strlen(topic);
}

} // namespace

void MqttService::begin(NotificationCenter& notifications)
{
    _lock = xSemaphoreCreateMutex();
    _notifications = &notifications;
    activeService = this;
    _client.setCallback(onMessage);
    _client.setKeepAlive(30);
    _client.setSocketTimeout(2);
    _client.setBufferSize(512);
    _network.setConnectionTimeout(3000);
    _network.setHandshakeTimeout(5);

    char clientId[24];
    std::snprintf(clientId, sizeof(clientId), "IoTCmd-%08lX",
        static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFF));
    _clientId = clientId;
}

void MqttService::notify(const String& title, const String& message)
{
    Guard guard(_lock);
    if (_pendingNotes.size() >= kMaxPendingNotes) {
        return;
    }
    _pendingNotes.push_back({title, message});
}

void MqttService::pump()
{
    std::vector<PendingNote> ready;
    {
        Guard guard(_lock);
        if (_pendingNotes.empty()) {
            return;
        }
        ready.swap(_pendingNotes);
    }
    for (const PendingNote& note : ready) {
        _notifications->push(note.title, note.message);
    }
}

void MqttService::service(const MqttSettings& settings)
{
    serviceInternal(settings);
    _connected.store(_connectionAttempted);
}

void MqttService::serviceInternal(const MqttSettings& settings)
{
    if (settingsChanged(settings)) {
        const bool connectionChanged =
            settings.host != _settings.host ||
            settings.port != _settings.port ||
            settings.username != _settings.username ||
            settings.password != _settings.password ||
            settings.rootCa != _settings.rootCa ||
            settings.telemetryTopic != _settings.telemetryTopic;
        const bool telemetryChanged = settings.telemetryTopic != _settings.telemetryTopic;
        const bool thresholdChanged =
            settings.alertEnabled != _settings.alertEnabled ||
            settings.alertThreshold != _settings.alertThreshold;
        const bool wasConnected = connectionChanged && _connectionAttempted;
        if (connectionChanged) {
            if (_connectionAttempted) {
                _client.disconnect();
            }
            _connectionAttempted = false;
        }
        _settings = settings;
        _hasCommandTopic.store(!_settings.commandTopic.isEmpty());
        if (_settings.rootCa.isEmpty()) {
            setStatus("Set broker root CA in Settings");
        } else {
            _network.setCACert(_settings.rootCa.c_str());
        }
        if (telemetryChanged) {
            Guard guard(_lock);
            _hasReading = false;
            _readings = {};
        }
        if (thresholdChanged) {
            _hasThresholdState = false;
        }
        if (connectionChanged) {
            _reportedConnection = false;
            _client.setServer(_settings.host.c_str(), _settings.port);
            if (_settings.host.isEmpty()) {
                setStatus("Configure MQTT in Settings");
            } else {
                setStatus("Waiting for Wi-Fi");
            }
        }
        if (wasConnected) {
            notify("MQTT disconnected", "Broker settings changed.");
        }
    }

    if (_settings.host.isEmpty()) {
        return;
    }
    if (_settings.rootCa.isEmpty()) {
        setStatus("Set broker root CA in Settings");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (_connectionAttempted) {
            _client.disconnect();
            _connectionAttempted = false;
        }
        setStatus("Waiting for Wi-Fi");
        return;
    }

    if (_connectionAttempted && !_client.connected()) {
        _connectionAttempted = false;
        _reportedConnection = false;
    }
    if (_connectionAttempted) {
        _client.loop();
        if (!_reportedConnection) {
            _reportedConnection = true;
            setStatus("Connected to " + _settings.host);
            notify("MQTT connected", "Connected to " + _settings.host);
        }
        const int8_t command = _pendingCommand.exchange(-1);
        if (command >= 0 && !_settings.commandTopic.isEmpty()) {
            if (!_client.publish(_settings.commandTopic.c_str(), command == 1 ? "ON" : "OFF", false)) {
                notify("MQTT command failed", "Check the broker and command topic.");
            }
        }
        return;
    }

    _reportedConnection = false;
    if (millis() - _lastReconnectAttempt < kReconnectIntervalMs) {
        return;
    }
    _lastReconnectAttempt = millis();
    setStatus("Connecting to " + _settings.host);

    bool connected = false;
    if (_settings.username.isEmpty()) {
        connected = _client.connect(_clientId.c_str());
    } else {
        connected = _client.connect(
            _clientId.c_str(),
            _settings.username.c_str(),
            _settings.password.c_str());
    }

    if (!connected) {
        setStatus("MQTT connection failed (" + String(_client.state()) + ")");
        return;
    }

    if (!_settings.telemetryTopic.isEmpty() && !_client.subscribe(_settings.telemetryTopic.c_str())) {
        _client.disconnect();
        _connectionAttempted = false;
        setStatus("Could not subscribe to telemetry topic");
        return;
    }

    _connectionAttempted = true;
    _reportedConnection = true;
    setStatus("Connected to " + _settings.host);
    notify("MQTT connected", "Connected to " + _settings.host);
}

bool MqttService::connected() const
{
    return _connected.load();
}

String MqttService::status() const
{
    Guard guard(_lock);
    return _status;
}

bool MqttService::hasReading() const
{
    Guard guard(_lock);
    return _hasReading;
}

float MqttService::reading() const
{
    Guard guard(_lock);
    return _reading;
}

std::array<TelemetryReading, kMaxTelemetryReadings> MqttService::readings() const
{
    Guard guard(_lock);
    return _readings;
}

const std::array<TelemetryReading, kMaxTelemetryReadings>& MqttService::readingsUnsafe() const
{
    return _readings;
}

uint32_t MqttService::readingRevision() const
{
    Guard guard(_lock);
    return _readingRevision;
}

bool MqttService::publishCommand(bool enabled)
{
    if (!_connected.load() || !_hasCommandTopic.load()) {
        return false;
    }
    _pendingCommand.store(enabled ? 1 : 0);
    return true;
}

void MqttService::onMessage(char* topic, uint8_t* payload, unsigned int length)
{
    if (activeService != nullptr) {
        activeService->handleMessage(topic, payload, length);
    }
}

void MqttService::handleMessage(const char* topic, const uint8_t* payload, unsigned int length)
{
    if (topic == nullptr) {
        Serial.println("[MQTT] Received message with null topic");
        return;
    }

    Serial.printf("[MQTT] RX topic='%s' length=%u\n", topic, length);

    if (!topicMatches(_settings.telemetryTopic, topic)) {
        Serial.printf("[MQTT] Ignored: topic does not match configured filter '%s'\n",
                      _settings.telemetryTopic.c_str());
        return;
    }

    if (length == 0) {
        Serial.println("[MQTT] Ignored: empty payload");
        return;
    }

    if (payload == nullptr) {
        Serial.println("[MQTT] Ignored: null payload");
        return;
    }

    if (length >= kMaxPayloadLength) {
        Serial.printf("[MQTT] Ignored: payload too large (%u bytes)\n", length);
        return;
    }

    char valueText[kMaxPayloadLength + 1];
    std::memcpy(valueText, payload, length);
    valueText[length] = '\0';

    JsonDocument document;
    const DeserializationError jsonError =
        deserializeJson(document, valueText, length);

    if (!jsonError && document.is<JsonObject>()) {
        JsonObject object = document.as<JsonObject>();
        size_t accepted = 0;

        for (JsonPair item : object) {
            if (item.key().size() > 24) {
                Serial.println("[MQTT] Skipped field: name exceeds 24 characters");
                continue;
            }

            if (!item.value().is<float>() &&
                !item.value().is<int>() &&
                !item.value().is<double>()) {
                Serial.printf("[MQTT] Skipped non-numeric field: %s\n",
                              item.key().c_str());
                continue;
            }

            const float value = item.value().as<float>();

            if (std::isfinite(value)) {
                updateReading(item.key().c_str(), value);
                Serial.printf("[MQTT] Accepted reading: %s=%.3f\n",
                              item.key().c_str(), value);
                ++accepted;
            } else {
                Serial.printf("[MQTT] Skipped non-finite field: %s\n",
                              item.key().c_str());
            }
        }

        Serial.printf("[MQTT] JSON object processed; accepted readings=%u\n",
                      static_cast<unsigned>(accepted));
    } else {
        char* end = nullptr;
        const float value = std::strtof(valueText, &end);

        if (end != valueText && *end == '\0' && std::isfinite(value)) {
            updateReading(_settings.tileLabel.c_str(), value);
            Serial.printf("[MQTT] Accepted numeric reading: %s=%.3f\n",
                          _settings.tileLabel.c_str(), value);
        } else {
            Serial.println(
                "[MQTT] Rejected payload: expected a number or JSON object");

            if (_lastNotificationAt == 0 ||
                millis() - _lastNotificationAt > kAlertRepeatIntervalMs) {
                notify("Invalid MQTT reading",
                       "Use a numeric value or a JSON object of numeric readings.");
                _lastNotificationAt = millis();
            }
        }
    }
}

void MqttService::updateReading(const char* name, float value)
{
    size_t index = _readings.size();
    bool crossedThreshold = false;

    {
        Guard guard(_lock);

        for (size_t candidate = 0; candidate < _readings.size(); ++candidate) {
            if (_readings[candidate].valid && _readings[candidate].name == name) {
                index = candidate;
                break;
            }

            if (index == _readings.size() && !_readings[candidate].valid) {
                index = candidate;
            }
        }

        if (index == _readings.size()) {
            index = 0;
            for (size_t candidate = 1; candidate < _readings.size(); ++candidate) {
                if (_readings[candidate].updatedAt < _readings[index].updatedAt) {
                    index = candidate;
                }
            }
        }

        crossedThreshold = _settings.alertEnabled &&
            value >= _settings.alertThreshold &&
            (!_readings[index].valid || _readings[index].name != name ||
                _readings[index].value < _settings.alertThreshold);

        _readings[index].name = name;
        _readings[index].value = value;
        _readings[index].updatedAt = millis();
        _readings[index].valid = true;
        _reading = value;
        _hasReading = true;
        ++_readingRevision;
    }

    if (crossedThreshold) {
        const String message = String(name) + " reached " + String(value, 2) +
            (_settings.unit.isEmpty() ? "" : " " + _settings.unit) +
            " (limit " + String(_settings.alertThreshold, 2) + ")";
        notify("MQTT threshold alert", message);
    }
}

bool MqttService::settingsChanged(const MqttSettings& settings) const
{
    return settings.host != _settings.host ||
        settings.port != _settings.port ||
        settings.username != _settings.username ||
        settings.password != _settings.password ||
        settings.rootCa != _settings.rootCa ||
        settings.telemetryTopic != _settings.telemetryTopic ||
        settings.commandTopic != _settings.commandTopic ||
        settings.tileLabel != _settings.tileLabel ||
        settings.unit != _settings.unit ||
        settings.visibleFields != _settings.visibleFields ||
        settings.alertThreshold != _settings.alertThreshold ||
        settings.alertEnabled != _settings.alertEnabled ||
        settings.tileEnabled != _settings.tileEnabled;
}

void MqttService::setStatus(const String& status)
{
    Guard guard(_lock);
    _status = status;
}
