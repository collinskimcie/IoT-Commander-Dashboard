#include "app/DeviceAdapter.h"

#include <algorithm>

bool DeviceAdapterRegistry::registerAdapter(DeviceAdapter& adapter)
{
    if (find(adapter.type()) != nullptr) {
        return false;
    }
    _adapters.push_back(&adapter);
    return true;
}

DeviceAdapter* DeviceAdapterRegistry::find(const String& type) const
{
    const auto found = std::find_if(_adapters.begin(), _adapters.end(), [&](const DeviceAdapter* adapter) {
        return type == adapter->type();
    });
    return found == _adapters.end() ? nullptr : *found;
}
