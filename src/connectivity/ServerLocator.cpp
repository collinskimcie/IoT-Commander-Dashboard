#include "app/ServerLocator.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

namespace {

// mDNS name of the machine that runs the backend and the MQTT broker, without ".local".
constexpr char kServerName[] = "santan";
constexpr size_t kMaxProfiles = 5;
constexpr uint32_t kResolveRetryMs = 60000;
constexpr uint32_t kLearnAfterMs = 45000;

int hostEnd(const String& url, int start)
{
    int end = start;
    while (end < static_cast<int>(url.length()) && url[end] != ':' && url[end] != '/') {
        ++end;
    }
    return end;
}

int hostStart(const String& url)
{
    const int scheme = url.indexOf("://");
    return scheme < 0 ? 0 : scheme + 3;
}

} // namespace

String ServerLocator::hostOf(const String& url)
{
    const int start = hostStart(url);
    return url.substring(start, hostEnd(url, start));
}

String ServerLocator::withHost(const String& url, const String& host)
{
    const int start = hostStart(url);
    return url.substring(0, start) + host + url.substring(hostEnd(url, start));
}

void ServerLocator::begin()
{
    loadProfiles();
}

void ServerLocator::update(const TerminalSettings& terminal, const MqttSettings& mqtt, bool backendOk)
{
    if (WiFi.status() != WL_CONNECTED) {
        _up = false;
        return;
    }
    const String ssid = WiFi.SSID();
    const uint32_t now = millis();
    if (!_up || ssid != _ssid) {
        _up = true;
        _ssid = ssid;
        _learned = false;
        _connectedAt = now;
        _nextResolveAt = now + 3000UL;
    }
    if (_resolveDone) {
        _resolveDone = false;
        _resolving = false;
        handleResolved(terminal, mqtt);
    }
    if (backendOk) {
        // Only learn once the status has settled, so a stale "ok" from the previous network is ignored.
        if (!_learned && now - _connectedAt > kLearnAfterMs) {
            rememberProfile(_ssid, hostOf(terminal.backendUrl), mqtt.host);
            _learned = true;
        }
        return;
    }
    _learned = false;
    if (!_resolving && static_cast<int32_t>(now - _nextResolveAt) >= 0) {
        startResolve();
    }
}

bool ServerLocator::takeChange(String& backendHost, String& mqttHost)
{
    if (!_changePending) {
        return false;
    }
    backendHost = _pendingBackendHost;
    mqttHost = _pendingMqttHost;
    _changePending = false;
    return true;
}

void ServerLocator::startResolve()
{
    _resolving = true;
    _resolveDone = false;
    _nextResolveAt = millis() + kResolveRetryMs;
    if (xTaskCreate(resolveTask, "srv_find", 6144, this, 1, nullptr) != pdPASS) {
        _resolving = false;
    }
}

void ServerLocator::resolveTask(void* parameter)
{
    auto* self = static_cast<ServerLocator*>(parameter);
    uint32_t found = 0;
    if (MDNS.begin("iot-commander")) {
        const IPAddress ip = MDNS.queryHost(kServerName, 3000);
        found = static_cast<uint32_t>(ip);
        MDNS.end();
    }
    self->_resolvedIp = found;
    self->_resolveDone = true;
    vTaskDelete(nullptr);
}

bool ServerLocator::sameSubnet(uint32_t ip) const
{
    const uint32_t mask = static_cast<uint32_t>(WiFi.subnetMask());
    const uint32_t local = static_cast<uint32_t>(WiFi.localIP());
    return (ip & mask) == (local & mask);
}

void ServerLocator::handleResolved(const TerminalSettings& terminal, const MqttSettings& mqtt)
{
    String backendHost;
    String mqttHost;
    const uint32_t ip = _resolvedIp;
    if (ip != 0 && sameSubnet(ip)) {
        backendHost = IPAddress(ip).toString();
        mqttHost = backendHost;
        Serial.printf("[Server] Found %s.local at %s\n", kServerName, backendHost.c_str());
    } else if (const Profile* profile = findProfile(_ssid)) {
        backendHost = profile->backendHost;
        mqttHost = profile->mqttHost;
        Serial.printf("[Server] Using addresses saved for %s\n", _ssid.c_str());
    } else {
        Serial.printf("[Server] No server found on %s; keeping current addresses\n", _ssid.c_str());
        return;
    }
    if (backendHost == hostOf(terminal.backendUrl) && mqttHost == mqtt.host) {
        return;
    }
    _pendingBackendHost = backendHost;
    _pendingMqttHost = mqttHost;
    _changePending = true;
}

const ServerLocator::Profile* ServerLocator::findProfile(const String& ssid) const
{
    for (const auto& profile : _profiles) {
        if (profile.ssid == ssid) {
            return &profile;
        }
    }
    return nullptr;
}

void ServerLocator::loadProfiles()
{
    _profiles.clear();
    Preferences prefs;
    if (!prefs.begin("srvprof", true)) {
        return;
    }
    const uint8_t count = prefs.getUChar("n", 0);
    for (uint8_t i = 0; i < count && i < kMaxProfiles; ++i) {
        char keySsid[4];
        char keyBackend[4];
        char keyMqtt[4];
        snprintf(keySsid, sizeof(keySsid), "s%u", static_cast<unsigned>(i));
        snprintf(keyBackend, sizeof(keyBackend), "b%u", static_cast<unsigned>(i));
        snprintf(keyMqtt, sizeof(keyMqtt), "m%u", static_cast<unsigned>(i));
        Profile profile;
        profile.ssid = prefs.getString(keySsid, "");
        profile.backendHost = prefs.getString(keyBackend, "");
        profile.mqttHost = prefs.getString(keyMqtt, "");
        if (!profile.ssid.isEmpty()) {
            _profiles.push_back(profile);
        }
    }
    prefs.end();
    Serial.printf("[Server] %u saved server profile(s) loaded\n", static_cast<unsigned>(_profiles.size()));
}

void ServerLocator::saveProfiles()
{
    Preferences prefs;
    if (!prefs.begin("srvprof", false)) {
        return;
    }
    prefs.putUChar("n", static_cast<uint8_t>(_profiles.size()));
    for (size_t i = 0; i < _profiles.size(); ++i) {
        char keySsid[4];
        char keyBackend[4];
        char keyMqtt[4];
        snprintf(keySsid, sizeof(keySsid), "s%u", static_cast<unsigned>(i));
        snprintf(keyBackend, sizeof(keyBackend), "b%u", static_cast<unsigned>(i));
        snprintf(keyMqtt, sizeof(keyMqtt), "m%u", static_cast<unsigned>(i));
        prefs.putString(keySsid, _profiles[i].ssid);
        prefs.putString(keyBackend, _profiles[i].backendHost);
        prefs.putString(keyMqtt, _profiles[i].mqttHost);
    }
    prefs.end();
}

void ServerLocator::rememberProfile(const String& ssid, const String& backendHost, const String& mqttHost)
{
    if (ssid.isEmpty() || backendHost.isEmpty() || mqttHost.isEmpty()) {
        return;
    }
    for (auto& profile : _profiles) {
        if (profile.ssid == ssid) {
            if (profile.backendHost == backendHost && profile.mqttHost == mqttHost) {
                return;
            }
            profile.backendHost = backendHost;
            profile.mqttHost = mqttHost;
            saveProfiles();
            Serial.printf("[Server] Updated saved addresses for %s\n", ssid.c_str());
            return;
        }
    }
    Profile profile;
    profile.ssid = ssid;
    profile.backendHost = backendHost;
    profile.mqttHost = mqttHost;
    _profiles.insert(_profiles.begin(), profile);
    if (_profiles.size() > kMaxProfiles) {
        _profiles.resize(kMaxProfiles);
    }
    saveProfiles();
    Serial.printf("[Server] Saved addresses for %s\n", ssid.c_str());
}
