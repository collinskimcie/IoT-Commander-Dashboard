#pragma once

#include "app/Domain.h"

class SettingsStore {
public:
    bool begin();
    bool darkTheme() const;
    void setDarkTheme(bool enabled);
    const String& wifiSsid() const;
    const String& wifiPassword() const;
    bool setWifiCredentials(const String& ssid, const String& password);

    const std::vector<SavedDevice>& devices() const;
    bool saveDevice(const NearbyDevice& device);
    bool removeDevice(const String& id);

    const MqttSettings& mqttSettings() const;
    bool setMqttSettings(const MqttSettings& settings);
    const TerminalSettings& terminalSettings() const;
    bool setTerminalSettings(const TerminalSettings& settings);

    // Bumped whenever the MQTT or terminal settings change, so consumers copy only on change.
    uint32_t revision() const { return _revision; }

private:
    void loadDevices();
    bool persistDevices();
    void loadMqttSettings();
    bool persistMqttSettings();
    void loadTerminalSettings();
    bool persistTerminalSettings();
    void loadWifiSettings();
    bool persistWifiSettings();

    bool _ready = false;
    String _wifiSsid;
    String _wifiPassword;
    bool _darkTheme = true;
    std::vector<SavedDevice> _devices;
    MqttSettings _mqttSettings;
    TerminalSettings _terminalSettings;
    uint32_t _revision = 1;
};
