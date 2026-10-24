#pragma once

#include "app/Domain.h"

struct BleResult {
    char id[20];
    char name[48];
    int rssi;
};

struct KnownNetwork {
    String ssid;
    String password;
};

class ConnectivityService {
public:
    void begin();
    void update();

    bool startWifiScan();
    bool startBluetoothScan();
    bool connectWifi(const String& ssid, const String& password);

    bool wifiConnected() const;
    bool wifiScanning() const;
    bool bluetoothScanning() const;
    String wifiSsid() const;
    String ipAddress() const;
    int wifiRssi() const;
    String statusMessage() const;
    const std::vector<NearbyDevice>& nearbyDevices() const;
    uint32_t scanRevision() const;

private:
    static void bluetoothScanTask(void* parameter);
    void finishWifiScan();
    void finishBluetoothScan();
    void loadKnownNetworks();
    void saveKnownNetworks();
    void rememberNetwork(const String& ssid, const String& password);
    void startAutoScan();
    void serviceAutoScan();
    void cancelAutoScan();

    bool _wifiScanning = false;
    bool _bluetoothScanning = false;
    bool _bluetoothScanFinished = false;
    bool _wifiConnecting = false;
    bool _previousWifiConnected = false;
    uint32_t _connectStartedAt = 0;
    uint32_t _nextWifiRetryAt = 0;
    String _lastSsid;
    String _lastPassword;
    std::vector<KnownNetwork> _known;
    bool _autoScanning = false;
    uint32_t _autoScanStartedAt = 0;
    uint8_t _failCount = 0;
    String _lastFailedSsid;
    uint32_t _scanRevision = 0;
    String _statusMessage = "Ready to discover systems";
    std::vector<NearbyDevice> _nearbyDevices;
    BleResult _bleResults[12] = {};
    uint8_t _bleResultCount = 0;
    portMUX_TYPE _bleMux = portMUX_INITIALIZER_UNLOCKED;
};
