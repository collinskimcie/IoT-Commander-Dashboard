#pragma once

#include <Arduino.h>

class TimeService {
public:
    void update(const String& timezone, bool wifiConnected);
    String localTime() const;
    bool synchronized() const;

private:
    String _timezone;
    bool _configured = false;
};
