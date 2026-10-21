#pragma once
#include <Arduino.h>

// Logs a warning when the enclosing scope takes longer than 300 ms.
class SlowGuard {
public:
    explicit SlowGuard(const char* name) : _name(name), _start(millis()) {}
    ~SlowGuard()
    {
        const uint32_t elapsed = millis() - _start;
        if (elapsed > 1000) {
            log_w("SLOW %s: %u ms (heap free=%u maxblock=%u)", _name,
                  static_cast<unsigned>(elapsed),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
        }
    }
private:
    const char* _name;
    uint32_t _start;
};
