#include "app/ConnectivityService.h"

#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiScanTimeoutMs = 20000;
constexpr size_t kMaxKnownNetworks = 5;
// Set to true to let the board join OPEN (password-less) networks when no known network is in range.
// Open networks are unauthenticated and often sit behind a login page, so this stays off by default.
constexpr bool kAllowOpenNetworks = false;
constexpr int kMinOpenRssi = -75;
uint32_t gWifiScanStartedAt = 0;
constexpr uint8_t kMaxBleDevices = 12;
constexpr uint32_t kBleScanSeconds = 6;
// BLE starts on demand and is released after each scan so its RAM goes back to TLS.
// If a second scan ever crashes, set this to false: BLE then stays up after the first scan.
constexpr bool kReleaseBleAfterScan = true;

bool containsDevice(const std::vector<NearbyDevice>& devices, Transport transport, const String& id)
{
    return std::any_of(devices.begin(), devices.end(), [&](const NearbyDevice& device) {
        return device.transport == transport && device.id == id;
    });
}

} // namespace

void ConnectivityService::begin()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    loadKnownNetworks();
}

void ConnectivityService::update()
{
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != _previousWifiConnected) {
        _statusMessage = connected ? "Connected to " + WiFi.SSID() : "Wi-Fi disconnected";
        _previousWifiConnected = connected;

        if (connected) {
            _failCount = 0;
            _lastFailedSsid = "";
            if (WiFi.SSID() == _lastSsid) {
                rememberNetwork(_lastSsid, _lastPassword);
            }
            Serial.printf(
                "[WiFi] Connected: SSID=%s, IP=%s, gateway=%s, RSSI=%d dBm\n",
                WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.RSSI());
        } else {
            Serial.printf("[WiFi] Disconnected; status=%d\n", static_cast<int>(WiFi.status()));
        }
    }

    if (_wifiScanning) {
        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
            finishWifiScan();
        } else if (millis() - gWifiScanStartedAt > kWifiScanTimeoutMs) {
            Serial.println("[WiFi] Scan timed out");
            esp_wifi_scan_stop();
            WiFi.scanDelete();
            _wifiScanning = false;
            _statusMessage = "Wi-Fi scan timed out";
        }
    }

    if (_bluetoothScanFinished) {
        finishBluetoothScan();
    }

    if (_autoScanning) {
        if (connected) {
            cancelAutoScan();
        } else {
            serviceAutoScan();
        }
    } else if (_wifiConnecting) {
        if (connected) {
            _wifiConnecting = false;
            _nextWifiRetryAt = millis() + 10000UL;
            _statusMessage = "Connected to " + WiFi.SSID();
        } else if (millis() - _connectStartedAt > kWifiConnectTimeoutMs) {
            const int wifiStatus = static_cast<int>(WiFi.status());
            Serial.printf(
                "[WiFi] Connection timeout: SSID=%s, status=%d; retry scheduled\n",
                _lastSsid.c_str(),
                wifiStatus);
            // Stop the failed station attempt before scheduling a retry.
            WiFi.disconnect(false, false);
            _wifiConnecting = false;
            _nextWifiRetryAt = millis() + 5000UL;
            if (_failCount < 100) {
                ++_failCount;
            }
            _lastFailedSsid = _lastSsid;
            _statusMessage = "Wi-Fi connection timed out (status " +
                String(wifiStatus) + "); retry scheduled";
        }
    } else if (connected && !_wifiScanning) {
        _statusMessage = "Connected to " + WiFi.SSID();
    } else if (!connected &&
               !_wifiScanning &&
               !_bluetoothScanning &&
               _failCount >= 1 &&
               !_known.empty() &&
               static_cast<int32_t>(millis() - _nextWifiRetryAt) >= 0) {
        startAutoScan();
    } else if (!connected &&
               !_lastSsid.isEmpty() &&
               !_wifiScanning &&
               !_bluetoothScanning &&
               static_cast<int32_t>(millis() - _nextWifiRetryAt) >= 0) {
        // Reset the station state before starting another connection attempt.
        WiFi.disconnect(false, false);
        delay(100);
        WiFi.mode(WIFI_STA);
        WiFi.begin(_lastSsid.c_str(), _lastPassword.c_str());
        _wifiConnecting = true;
        _connectStartedAt = millis();
        _statusMessage = "Reconnecting to " + _lastSsid;
    }
}

void ConnectivityService::loadKnownNetworks()
{
    _known.clear();
    Preferences prefs;
    if (!prefs.begin("wifinets", true)) {
        return;
    }
    const uint8_t count = prefs.getUChar("n", 0);
    for (uint8_t i = 0; i < count && i < kMaxKnownNetworks; ++i) {
        char ks[4];
        char kp[4];
        snprintf(ks, sizeof(ks), "s%u", static_cast<unsigned>(i));
        snprintf(kp, sizeof(kp), "p%u", static_cast<unsigned>(i));
        KnownNetwork net;
        net.ssid = prefs.getString(ks, "");
        net.password = prefs.getString(kp, "");
        if (!net.ssid.isEmpty()) {
            _known.push_back(net);
        }
    }
    prefs.end();
    Serial.printf("[WiFi] %u known network(s) loaded\n", static_cast<unsigned>(_known.size()));
}

void ConnectivityService::saveKnownNetworks()
{
    Preferences prefs;
    if (!prefs.begin("wifinets", false)) {
        return;
    }
    prefs.putUChar("n", static_cast<uint8_t>(_known.size()));
    for (size_t i = 0; i < _known.size(); ++i) {
        char ks[4];
        char kp[4];
        snprintf(ks, sizeof(ks), "s%u", static_cast<unsigned>(i));
        snprintf(kp, sizeof(kp), "p%u", static_cast<unsigned>(i));
        prefs.putString(ks, _known[i].ssid);
        prefs.putString(kp, _known[i].password);
    }
    prefs.end();
}

void ConnectivityService::rememberNetwork(const String& ssid, const String& password)
{
    // Open networks are never saved.
    if (ssid.isEmpty() || password.isEmpty()) {
        return;
    }
    for (size_t i = 0; i < _known.size(); ++i) {
        if (_known[i].ssid == ssid) {
            if (_known[i].password == password) {
                return;
            }
            _known[i].password = password;
            saveKnownNetworks();
            return;
        }
    }
    KnownNetwork net;
    net.ssid = ssid;
    net.password = password;
    _known.insert(_known.begin(), net);
    if (_known.size() > kMaxKnownNetworks) {
        _known.resize(kMaxKnownNetworks);
    }
    saveKnownNetworks();
    Serial.printf("[WiFi] Remembered network: %s\n", ssid.c_str());
}

void ConnectivityService::cancelAutoScan()
{
    if (!_autoScanning) {
        return;
    }
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    _autoScanning = false;
}

void ConnectivityService::startAutoScan()
{
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        delay(300);
    }
    WiFi.scanDelete();
    if (WiFi.scanNetworks(true, true) == WIFI_SCAN_FAILED) {
        _nextWifiRetryAt = millis() + 10000UL;
        return;
    }
    _autoScanning = true;
    _autoScanStartedAt = millis();
    _statusMessage = "Looking for a known Wi-Fi network";
    Serial.println("[WiFi] Auto scan started");
}

void ConnectivityService::serviceAutoScan()
{
    const int count = WiFi.scanComplete();
    if (count == WIFI_SCAN_RUNNING) {
        if (millis() - _autoScanStartedAt > kWifiScanTimeoutMs) {
            cancelAutoScan();
            _nextWifiRetryAt = millis() + 10000UL;
        }
        return;
    }
    _autoScanning = false;

    // The network that just failed is penalised so another candidate gets a turn.
    int bestKnownScore = -10000;
    int bestOpenScore = -10000;
    String knownSsid, knownPassword, openSsid;
    for (int i = 0; i < count; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) {
            continue;
        }
        const int rssi = WiFi.RSSI(i);
        const int score = rssi - (ssid == _lastFailedSsid ? 100 : 0);
        bool known = false;
        for (const auto& net : _known) {
            if (net.ssid == ssid) {
                known = true;
                if (score > bestKnownScore) {
                    bestKnownScore = score;
                    knownSsid = net.ssid;
                    knownPassword = net.password;
                }
                break;
            }
        }
        if (!known && kAllowOpenNetworks &&
            WiFi.encryptionType(i) == WIFI_AUTH_OPEN &&
            rssi >= kMinOpenRssi && score > bestOpenScore) {
            bestOpenScore = score;
            openSsid = ssid;
        }
    }
    WiFi.scanDelete();

    String pickSsid, pickPassword;
    if (!knownSsid.isEmpty()) {
        pickSsid = knownSsid;
        pickPassword = knownPassword;
    } else if (!openSsid.isEmpty()) {
        pickSsid = openSsid;
    }
    if (pickSsid.isEmpty()) {
        Serial.printf("[WiFi] No known network in range (%d seen)\n", count);
        _statusMessage = "No known Wi-Fi network in range";
        _nextWifiRetryAt = millis() + 15000UL;
        return;
    }

    _lastSsid = pickSsid;
    _lastPassword = pickPassword;
    Serial.printf("[WiFi] Auto-connecting: SSID=%s (%s)\n",
                  pickSsid.c_str(), pickPassword.isEmpty() ? "open" : "known");
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(_lastSsid.c_str(), _lastPassword.c_str());
    _wifiConnecting = true;
    _connectStartedAt = millis();
    _nextWifiRetryAt = _connectStartedAt + kWifiConnectTimeoutMs + 5000UL;
    _statusMessage = "Connecting to " + pickSsid;
}

bool ConnectivityService::startWifiScan()
{
    cancelAutoScan();
    if (_wifiScanning || _bluetoothScanning) {
        _statusMessage = "Wait for the current scan to finish";
        return false;
    }

    // Pause automatic reconnect attempts while scanning.
    _nextWifiRetryAt = millis() + 30000UL;
    _wifiConnecting = false;

    // Keep the radio up so MQTT and the backend stay connected during the scan.
    // Only a half-finished connect attempt is stopped, since it cannot run alongside a scan.
    // Auto-reconnect is off so the driver does not restart the attempt and make the scan fail.
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        delay(300);
    }
    WiFi.scanDelete();

    // Asynchronous: returns immediately and update() collects the result.
    const int result = WiFi.scanNetworks(true, true);
    if (result == WIFI_SCAN_FAILED) {
        _statusMessage = "Wi-Fi scan failed";
        Serial.printf("[WiFi] Scan start failed; status=%d\n", static_cast<int>(WiFi.status()));
        return false;
    }

    _wifiScanning = true;
    gWifiScanStartedAt = millis();
    _statusMessage = "Scanning for Wi-Fi networks";
    Serial.println("[WiFi] Async scan started");
    return true;
}

bool ConnectivityService::startBluetoothScan()
{
    cancelAutoScan();
    if (_wifiScanning || _bluetoothScanning || _wifiConnecting) {
        _statusMessage = "Wait for the current scan to finish";
        return false;
    }

    _bleResultCount = 0;
    _bluetoothScanFinished = false;
    _bluetoothScanning = true;
    _statusMessage = "Scanning for Bluetooth devices";
    if (!BLEDevice::getInitialized()) {
        const uint32_t heapBefore = ESP.getFreeHeap();
        BLEDevice::init("IoT Commander");
        log_i("BLE started: heap free %u -> %u (maxblock %u)",
              static_cast<unsigned>(heapBefore),
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
    const BaseType_t created = xTaskCreatePinnedToCore(
        bluetoothScanTask, "ble_scan", 4096, this, 1, nullptr, 0);
    if (created != pdPASS) {
        _bluetoothScanning = false;
        _statusMessage = "Could not start Bluetooth scan";
        return false;
    }
    return true;
}

bool ConnectivityService::connectWifi(const String& ssid, const String& password)
{
    cancelAutoScan();
    if (ssid.isEmpty() || _wifiScanning || _bluetoothScanning) {
        _statusMessage = "Select a network after discovery has finished";
        return false;
    }

    // Replace the active target and reset any previous station attempt.
    _lastSsid = ssid;
    _lastPassword = password;
    _wifiConnecting = false;

    Serial.printf("[WiFi] Starting connection attempt: SSID=%s\n", ssid.c_str());
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(_lastSsid.c_str(), _lastPassword.c_str());
    _wifiConnecting = true;
    _connectStartedAt = millis();
    _nextWifiRetryAt = _connectStartedAt + kWifiConnectTimeoutMs + 5000UL;
    _statusMessage = "Connecting to " + ssid;
    return true;
}

bool ConnectivityService::wifiConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

bool ConnectivityService::wifiScanning() const
{
    return _wifiScanning;
}

bool ConnectivityService::bluetoothScanning() const
{
    return _bluetoothScanning;
}

String ConnectivityService::wifiSsid() const
{
    return wifiConnected() ? WiFi.SSID() : String();
}

String ConnectivityService::ipAddress() const
{
    return wifiConnected() ? WiFi.localIP().toString() : String();
}

int ConnectivityService::wifiRssi() const
{
    return wifiConnected() ? WiFi.RSSI() : 0;
}

String ConnectivityService::statusMessage() const
{
    return _statusMessage;
}

const std::vector<NearbyDevice>& ConnectivityService::nearbyDevices() const
{
    return _nearbyDevices;
}

uint32_t ConnectivityService::scanRevision() const
{
    return _scanRevision;
}

void ConnectivityService::finishWifiScan()
{
    const int count = WiFi.scanComplete();
    _wifiScanning = false;
    if (count < 0) {
        _statusMessage = "Wi-Fi scan failed";
        return;
    }

    // Replace previous Wi-Fi results, keeping discovered Bluetooth devices.
    _nearbyDevices.erase(
        std::remove_if(
            _nearbyDevices.begin(),
            _nearbyDevices.end(),
            [](const NearbyDevice& device) {
                return device.transport == Transport::Wifi;
            }),
        _nearbyDevices.end());

    for (int index = 0; index < count; ++index) {
        const String ssid = WiFi.SSID(index);
        if (ssid.isEmpty()) {
            continue;
        }

        _nearbyDevices.push_back({
            ssid,
            ssid,
            Transport::Wifi,
            WiFi.RSSI(index),
            WiFi.encryptionType(index) != WIFI_AUTH_OPEN
        });
    }
    WiFi.scanDelete();
    ++_scanRevision;
    _statusMessage = "Wi-Fi discovery complete";
}

void ConnectivityService::finishBluetoothScan()
{
    BleResult results[kMaxBleDevices];
    uint8_t resultCount = 0;
    portENTER_CRITICAL(&_bleMux);
    resultCount = _bleResultCount;
    std::memcpy(results, _bleResults, sizeof(BleResult) * resultCount);
    _bluetoothScanFinished = false;
    _bluetoothScanning = false;
    portEXIT_CRITICAL(&_bleMux);

    for (uint8_t index = 0; index < resultCount; ++index) {
        const String id = results[index].id;
        const String name = results[index].name[0] == '\0' ? id : String(results[index].name);
        if (!containsDevice(_nearbyDevices, Transport::Bluetooth, id)) {
            _nearbyDevices.push_back({
                id,
                name,
                Transport::Bluetooth,
                results[index].rssi,
                false
            });
        }
    }

    ++_scanRevision;
    _statusMessage = resultCount == 0 ? "No Bluetooth devices found" : "Bluetooth discovery complete";
}

void ConnectivityService::bluetoothScanTask(void* parameter)
{
    auto* service = static_cast<ConnectivityService*>(parameter);
    if (!BLEDevice::getInitialized()) {
        // BLE failed to start (most likely out of memory): finish with no results.
        portENTER_CRITICAL(&service->_bleMux);
        service->_bleResultCount = 0;
        service->_bluetoothScanFinished = true;
        portEXIT_CRITICAL(&service->_bleMux);
        vTaskDelete(nullptr);
    }
    BLEScan* scanner = BLEDevice::getScan();
    scanner->setActiveScan(true);
    scanner->setInterval(100);
    scanner->setWindow(80);

    BLEScanResults* results = scanner->start(kBleScanSeconds, false);
    const int found = results == nullptr ? 0 : results->getCount();
    BleResult discovered[kMaxBleDevices] = {};
    uint8_t discoveredCount = 0;
    for (int index = 0; index < found && discoveredCount < kMaxBleDevices; ++index) {
        BLEAdvertisedDevice device = results->getDevice(index);
        BleResult& result = discovered[discoveredCount++];
        const String address = device.getAddress().toString();
        const String name = device.haveName() ? device.getName() : "";
        std::snprintf(result.id, sizeof(result.id), "%s", address.c_str());
        std::snprintf(result.name, sizeof(result.name), "%s", name.c_str());
        result.rssi = device.getRSSI();
    }

    if (results != nullptr) {
        scanner->clearResults();
    }
    if (kReleaseBleAfterScan) {
        BLEDevice::deinit(false);
        log_i("BLE released: heap free=%u maxblock=%u",
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }

    // Publish the results only after BLE is released, so a new scan cannot start mid-teardown.
    portENTER_CRITICAL(&service->_bleMux);
    std::memcpy(service->_bleResults, discovered, sizeof(BleResult) * discoveredCount);
    service->_bleResultCount = discoveredCount;
    service->_bluetoothScanFinished = true;
    portEXIT_CRITICAL(&service->_bleMux);
    vTaskDelete(nullptr);
}
