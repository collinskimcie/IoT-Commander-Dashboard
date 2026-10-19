#include "app/TimeService.h"

#include <ctime>
#include <cstdlib>
#include <time.h>

void TimeService::update(const String& timezone, bool wifiConnected)
{
    if (_timezone != timezone) {
        _timezone = timezone;
        _configured = false;
    }
    if (!wifiConnected) {
        // Reconfigure NTP when connectivity returns.
        _configured = false;
        return;
    }

    if (!_configured && !_timezone.isEmpty()) {
        configTzTime(_timezone.c_str(), "pool.ntp.org", "time.google.com");
        _configured = true;
    }
}

String TimeService::localTime() const
{
    if (!synchronized()) {
        return "--:--";
    }
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    char text[24];
    std::strftime(text, sizeof(text), "%H:%M  %d %b", &local);
    return text;
}

bool TimeService::synchronized() const
{
    return time(nullptr) > 1700000000;
}
