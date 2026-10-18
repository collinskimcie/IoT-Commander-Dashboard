#pragma once

#include "app/Domain.h"

class SettingsStore {
public:
    bool begin();
    bool darkTheme() const;
    void setDarkTheme(bool enabled);

    const std::vector<SavedDevice>& devices() const;
    bool saveDevice(const NearbyDevice& device);
    bool removeDevice(const String& id);

    const MqttSettings& mqttSettings() const;
    bool setMqttSettings(const MqttSettings& settings);
    const TerminalSettings& terminalSettings() const;
    bool setTerminalSettings(const TerminalSettings& settings);

private:
    void loadDevices();
    bool persistDevices();
    void loadMqttSettings();
    bool persistMqttSettings();
    void loadTerminalSettings();
    bool persistTerminalSettings();

    bool _ready = false;
    bool _darkTheme = true;
    std::vector<SavedDevice> _devices;
    MqttSettings _mqttSettings;
    TerminalSettings _terminalSettings;
};
