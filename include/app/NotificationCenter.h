#pragma once

#include "app/Domain.h"

class NotificationCenter {
public:
    void push(const String& title, const String& message);
    void markAllRead();
    void clear();

    const std::vector<Notification>& items() const;
    size_t unreadCount() const;
    uint32_t revision() const;

private:
    uint32_t _nextId = 1;
    uint32_t _revision = 0;
    std::vector<Notification> _items;
};
