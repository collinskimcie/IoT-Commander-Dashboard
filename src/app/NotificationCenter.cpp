#include "app/NotificationCenter.h"

void NotificationCenter::push(const String& title, const String& message)
{
    _items.insert(_items.begin(), {_nextId++, title, message, millis(), true});
    if (_items.size() > 20) {
        _items.pop_back();
    }
    ++_revision;
}

void NotificationCenter::markAllRead()
{
    for (Notification& item : _items) {
        item.unread = false;
    }
}

void NotificationCenter::clear()
{
    _items.clear();
    ++_revision;
}

const std::vector<Notification>& NotificationCenter::items() const
{
    return _items;
}

size_t NotificationCenter::unreadCount() const
{
    size_t count = 0;
    for (const Notification& item : _items) {
        count += item.unread ? 1 : 0;
    }
    return count;
}

uint32_t NotificationCenter::revision() const
{
    return _revision;
}
