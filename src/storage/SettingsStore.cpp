#include "app/MqttLocalConfig.h"
#include "app/SettingsStore.h"

#include <Preferences.h>
#include <algorithm>
#include <cmath>

namespace {

constexpr uint8_t kMaxSavedDevices = 8;
Preferences preferences;

String deviceKey(uint8_t index, const char* field)
{
    return "d" + String(index) + "_" + field;
}

String storedString(const char* key, const char* fallback)
{
    return preferences.isKey(key) ? preferences.getString(key) : String(fallback);
}

// Overwrites in place. NVS writes the new entry before retiring the old one, so a power
// cut leaves the old or the new value, never nothing (removing the key first could lose the CA).
bool storeString(const char* key, const String& value)
{
    const size_t written = preferences.putString(key, value);
    if (value.isEmpty()) {
        return written == 0 && preferences.isKey(key);
    }
    return written == value.length() && preferences.getString(key, "") == value;
}

} // namespace

bool SettingsStore::begin()
{
    _ready = preferences.begin("commander", false);
    if (!_ready) {
        return false;
    }

    _darkTheme = preferences.getBool("dark", true);
    loadDevices();
    loadMqttSettings();
    loadTerminalSettings();
    loadWifiSettings();
    return true;
}

bool SettingsStore::darkTheme() const
{
    return _darkTheme;
}

void SettingsStore::setDarkTheme(bool enabled)
{
    _darkTheme = enabled;
    if (_ready) {
        preferences.putBool("dark", enabled);
    }
}

const String& SettingsStore::wifiSsid() const
{
    return _wifiSsid;
}

const String& SettingsStore::wifiPassword() const
{
    return _wifiPassword;
}

bool SettingsStore::setWifiCredentials(const String& ssid, const String& password)
{
    if (!_ready || ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
        return false;
    }

    const String previousSsid = _wifiSsid;
    const String previousPassword = _wifiPassword;
    _wifiSsid = ssid;
    _wifiPassword = password;
    if (!persistWifiSettings()) {
        _wifiSsid = previousSsid;
        _wifiPassword = previousPassword;
        return false;
    }
    return true;
}

const std::vector<SavedDevice>& SettingsStore::devices() const
{
    return _devices;
}

bool SettingsStore::saveDevice(const NearbyDevice& device)
{
    for (SavedDevice& saved : _devices) {
        if (saved.id == device.id && saved.transport == device.transport) {
            saved.name = device.name;
            return persistDevices();
        }
    }

    if (_devices.size() >= kMaxSavedDevices) {
        return false;
    }

    _devices.push_back({device.id, device.name, device.transport});
    if (!persistDevices()) {
        _devices.pop_back();
        return false;
    }
    return true;
}

bool SettingsStore::removeDevice(const String& id)
{
    const auto found = std::find_if(_devices.begin(), _devices.end(), [&](const SavedDevice& device) {
        return device.id == id;
    });
    if (found == _devices.end()) {
        return false;
    }

    _devices.erase(found);
    return persistDevices();
}

const MqttSettings& SettingsStore::mqttSettings() const
{
    return _mqttSettings;
}

bool SettingsStore::setMqttSettings(const MqttSettings& settings)
{
    if (settings.host.length() > 64 ||
        settings.username.length() > 32 ||
        settings.password.length() > 64 ||
        settings.rootCa.length() > 2048 ||
        settings.telemetryTopic.length() > 96 ||
        settings.commandTopic.length() > 96 ||
        settings.tileLabel.length() > 20 ||
        settings.unit.length() > 8 ||
        settings.visibleFields.length() > 64 ||
        settings.port == 0 ||
        !std::isfinite(settings.alertThreshold)) {
        return false;
    }

    const MqttSettings previous = _mqttSettings;
    _mqttSettings = settings;
    if (!persistMqttSettings()) {
        _mqttSettings = previous;
        return false;
    }
    return true;
}

const TerminalSettings& SettingsStore::terminalSettings() const
{
    return _terminalSettings;
}

bool SettingsStore::setTerminalSettings(const TerminalSettings& settings)
{
    if (settings.backendUrl.length() > 128 ||
        settings.backendApiKey.length() > 64 ||
        settings.backendRootCa.length() > 2048 ||
        settings.timezone.length() > 40) {
        return false;
    }

    const TerminalSettings previous = _terminalSettings;
    _terminalSettings = settings;
    if (!persistTerminalSettings()) {
        _terminalSettings = previous;
        return false;
    }
    return true;
}

void SettingsStore::loadWifiSettings()
{
    _wifiSsid = preferences.getString("wifi_ssid", "");
    _wifiPassword = preferences.getString("wifi_pass", "");
}

bool SettingsStore::persistWifiSettings()
{
    if (!_ready) {
        return false;
    }

    preferences.putString("wifi_ssid", _wifiSsid);
    preferences.putString("wifi_pass", _wifiPassword);
    return preferences.isKey("wifi_ssid") &&
        preferences.getString("wifi_ssid", "") == _wifiSsid &&
        preferences.isKey("wifi_pass") &&
        preferences.getString("wifi_pass", "") == _wifiPassword;
}

void SettingsStore::loadDevices()
{
    const uint8_t count = preferences.getUChar("device_count", 0);
    _devices.clear();
    for (uint8_t index = 0; index < count && index < kMaxSavedDevices; ++index) {
        const String id = preferences.getString(deviceKey(index, "id").c_str(), "");
        if (id.isEmpty()) {
            continue;
        }
        _devices.push_back({
            id,
            preferences.getString(deviceKey(index, "name").c_str(), id),
            preferences.getUChar(deviceKey(index, "transport").c_str(), 0) == 0
                ? Transport::Wifi
                : Transport::Bluetooth
        });
    }
}

void SettingsStore::loadMqttSettings()
{
    _mqttSettings.host = storedString("mq_host", "");
    _mqttSettings.port = preferences.isKey("mq_port") ? preferences.getUShort("mq_port") : 1883;
    _mqttSettings.username = storedString("mq_user", "");
    _mqttSettings.password = storedString("mq_pass", "");
    _mqttSettings.rootCa = storedString("mq_ca", "");
    _mqttSettings.telemetryTopic = storedString("mq_tele", "");
    _mqttSettings.commandTopic = storedString("mq_cmd", "");
    _mqttSettings.tileLabel = storedString("mq_label", "Sensor");
    _mqttSettings.unit = storedString("mq_unit", "");
    _mqttSettings.visibleFields = storedString("mq_fields", "");
    _mqttSettings.alertThreshold = preferences.isKey("mq_limit") ? preferences.getFloat("mq_limit") : 0.0F;
    _mqttSettings.alertEnabled = preferences.isKey("mq_alert") && preferences.getBool("mq_alert");
    _mqttSettings.tileEnabled = !preferences.isKey("mq_tile") || preferences.getBool("mq_tile");

    // Seed the build-time broker configuration only on a terminal that has never saved
    // MQTT settings. Anything the user saved (even with an empty CA) is left alone.
    const bool seeded = !preferences.isKey("mq_host");
    if (seeded) {
        _mqttSettings.host = MqttLocalConfig::host;
        _mqttSettings.port = MqttLocalConfig::port;
        _mqttSettings.username = MqttLocalConfig::username;
        _mqttSettings.password = MqttLocalConfig::password;
        _mqttSettings.rootCa = MqttLocalConfig::rootCa;
        _mqttSettings.telemetryTopic = MqttLocalConfig::telemetryTopic;
    }
    log_i("settings: mqtt %s, host_set=%d ca_bytes=%u",
        seeded ? "seeded from build config" : "loaded from NVS",
        _mqttSettings.host.isEmpty() ? 0 : 1,
        static_cast<unsigned>(_mqttSettings.rootCa.length()));
}

bool SettingsStore::persistMqttSettings()
{
    if (!_ready) {
        return false;
    }

    bool success = true;
    success &= storeString("mq_host", _mqttSettings.host);
    success &= preferences.putUShort("mq_port", _mqttSettings.port) > 0;
    success &= storeString("mq_user", _mqttSettings.username);
    success &= storeString("mq_pass", _mqttSettings.password);
    success &= storeString("mq_ca", _mqttSettings.rootCa);
    success &= storeString("mq_tele", _mqttSettings.telemetryTopic);
    success &= storeString("mq_cmd", _mqttSettings.commandTopic);
    success &= storeString("mq_label", _mqttSettings.tileLabel);
    success &= storeString("mq_unit", _mqttSettings.unit);
    success &= storeString("mq_fields", _mqttSettings.visibleFields);
    success &= preferences.putFloat("mq_limit", _mqttSettings.alertThreshold) > 0;
    success &= preferences.putBool("mq_alert", _mqttSettings.alertEnabled);
    success &= preferences.putBool("mq_tile", _mqttSettings.tileEnabled);
    return success;
}

void SettingsStore::loadTerminalSettings()
{
    _terminalSettings.backendUrl = storedString("api_base", "");
    _terminalSettings.backendApiKey = storedString("api_key", "");
    _terminalSettings.backendRootCa = storedString("api_ca", "");
    _terminalSettings.timezone = storedString("timezone", "EAT-3");
    // UTC-3 is the equivalent legacy POSIX offset (UTC+3).
    if (_terminalSettings.timezone == "UTC-3") {
        _terminalSettings.timezone = "EAT-3";
    }
}

bool SettingsStore::persistTerminalSettings()
{
    if (!_ready) {
        return false;
    }

    bool success = true;
    success &= storeString("api_base", _terminalSettings.backendUrl);
    success &= storeString("api_key", _terminalSettings.backendApiKey);
    success &= storeString("api_ca", _terminalSettings.backendRootCa);
    success &= storeString("timezone", _terminalSettings.timezone);
    return success;
}

bool SettingsStore::persistDevices()
{
    if (!_ready) {
        return false;
    }

    for (uint8_t index = 0; index < kMaxSavedDevices; ++index) {
        preferences.remove(deviceKey(index, "id").c_str());
        preferences.remove(deviceKey(index, "name").c_str());
        preferences.remove(deviceKey(index, "transport").c_str());
    }

    for (uint8_t index = 0; index < _devices.size(); ++index) {
        const SavedDevice& device = _devices[index];
        if (preferences.putString(deviceKey(index, "id").c_str(), device.id) == 0 ||
            preferences.putString(deviceKey(index, "name").c_str(), device.name) == 0 ||
            preferences.putUChar(deviceKey(index, "transport").c_str(),
                device.transport == Transport::Wifi ? 0 : 1) == 0) {
            return false;
        }
    }

    return preferences.putUChar("device_count", _devices.size()) > 0;
}
