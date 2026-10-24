#pragma once

#include <Arduino.h>
#include <vector>

#include "app/Domain.h"

// Finds the backend/MQTT machine on whatever Wi-Fi network the board joined.
// Order: mDNS name lookup (same subnet only) -> addresses saved for this SSID -> current settings.
class ServerLocator {
public:
    void begin();
    // Call from the main loop. backendOk = the backend answered recently.
    void update(const TerminalSettings& terminal, const MqttSettings& mqtt, bool backendOk);
    // True once when new addresses should be applied; fills in the hosts to use.
    bool takeChange(String& backendHost, String& mqttHost);

    static String hostOf(const String& url);
    static String withHost(const String& url, const String& host);

private:
    struct Profile {
        String ssid;
        String backendHost;
        String mqttHost;
    };

    static void resolveTask(void* parameter);
    void startResolve();
    void handleResolved(const TerminalSettings& terminal, const MqttSettings& mqtt);
    bool sameSubnet(uint32_t ip) const;
    void loadProfiles();
    void saveProfiles();
    void rememberProfile(const String& ssid, const String& backendHost, const String& mqttHost);
    const Profile* findProfile(const String& ssid) const;

    std::vector<Profile> _profiles;
    String _ssid;
    bool _up = false;
    bool _learned = false;
    bool _resolving = false;
    volatile bool _resolveDone = false;
    volatile uint32_t _resolvedIp = 0;
    uint32_t _connectedAt = 0;
    uint32_t _nextResolveAt = 0;
    bool _changePending = false;
    String _pendingBackendHost;
    String _pendingMqttHost;
};
