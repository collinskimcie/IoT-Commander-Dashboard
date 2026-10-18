#pragma once

#include "app/Domain.h"

#include <vector>

struct DeviceTelemetry {
    String summary;
    String detail;
    uint32_t updatedAt;
};

class DeviceAdapter {
public:
    virtual ~DeviceAdapter() = default;
    virtual const char* type() const = 0;
    virtual bool connect(const SavedDevice& device) = 0;
    virtual void disconnect(const String& deviceId) = 0;
    virtual bool sendCommand(const String& deviceId, const String& command, const String& value) = 0;
    virtual bool readTelemetry(const String& deviceId, DeviceTelemetry& telemetry) = 0;
};

class DeviceAdapterRegistry {
public:
    bool registerAdapter(DeviceAdapter& adapter);
    DeviceAdapter* find(const String& type) const;

private:
    std::vector<DeviceAdapter*> _adapters;
};
