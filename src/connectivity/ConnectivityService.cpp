#include "app/ConnectivityService.h"

#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint8_t kMaxBleDevices = 12;
constexpr uint32_t kBleScanSeconds = 6;

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
    BLEDevice::init("IoT Commander");
}

void ConnectivityService::update()
{
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != _previousWifiConnected) {
        _statusMessage = connected ? "Connected to " + WiFi.SSID() : "Wi-Fi disconnected";
        _previousWifiConnected = connected;
    }

    if (_wifiScanning && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        finishWifiScan();
    }

    if (_bluetoothScanFinished) {
        finishBluetoothScan();
    }

    if (_wifiConnecting) {
        if (connected) {
            _wifiConnecting = false;
            _nextWifiRetryAt = millis() + 10000UL;
            _statusMessage = "Connected to " + WiFi.SSID();
        } else if (millis() - _connectStartedAt > kWifiConnectTimeoutMs) {
            _wifiConnecting = false;
            _nextWifiRetryAt = millis() + 5000UL;
            _statusMessage = "Wi-Fi connection timed out; retry scheduled";
        }
    } else if (connected && !_wifiScanning) {
        _statusMessage = "Connected to " + WiFi.SSID();
    } else if (!connected &&
               !_lastSsid.isEmpty() &&
               !_wifiScanning &&
               !_bluetoothScanning &&
               static_cast<int32_t>(millis() - _nextWifiRetryAt) >= 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(_lastSsid.c_str(), _lastPassword.c_str());
        _wifiConnecting = true;
        _connectStartedAt = millis();
        _statusMessage = "Reconnecting to " + _lastSsid;
    }
}

bool ConnectivityService::startWifiScan()
{
    if (_wifiScanning || _bluetoothScanning || _wifiConnecting) {
        _statusMessage = "Wait for the current scan to finish";
        return false;
    }

    WiFi.mode(WIFI_STA);
    const int result = WiFi.scanNetworks(true, true);
    if (result == WIFI_SCAN_FAILED) {
        _statusMessage = "Could not start Wi-Fi scan";
        return false;
    }

    _wifiScanning = true;
    _statusMessage = "Scanning for Wi-Fi networks";
    return true;
}

bool ConnectivityService::startBluetoothScan()
{
    if (_wifiScanning || _bluetoothScanning || _wifiConnecting) {
        _statusMessage = "Wait for the current scan to finish";
        return false;
    }

    _bleResultCount = 0;
    _bluetoothScanFinished = false;
    _bluetoothScanning = true;
    _statusMessage = "Scanning for Bluetooth devices";
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
    if (ssid.isEmpty() || _wifiScanning || _bluetoothScanning) {
        _statusMessage = "Select a network after discovery has finished";
        return false;
    }

    _lastSsid = ssid;
    _lastPassword = password;
    WiFi.disconnect(false);
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

    for (int index = 0; index < count; ++index) {
        const String ssid = WiFi.SSID(index);
        if (ssid.isEmpty()) {
            continue;
        }

        if (!containsDevice(_nearbyDevices, Transport::Wifi, ssid)) {
            _nearbyDevices.push_back({
                ssid,
                ssid,
                Transport::Wifi,
                WiFi.RSSI(index),
                WiFi.encryptionType(index) != WIFI_AUTH_OPEN
            });
        }
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

    portENTER_CRITICAL(&service->_bleMux);
    std::memcpy(service->_bleResults, discovered, sizeof(BleResult) * discoveredCount);
    service->_bleResultCount = discoveredCount;
    service->_bluetoothScanFinished = true;
    portEXIT_CRITICAL(&service->_bleMux);
    if (results != nullptr) {
        scanner->clearResults();
    }
    vTaskDelete(nullptr);
}
