#include "platform/ScreenPower.h"

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>

namespace {

#ifndef SCREEN_DIM_AFTER_MS
#define SCREEN_DIM_AFTER_MS 60000UL
#endif

constexpr uint32_t kDimAfterMs = SCREEN_DIM_AFTER_MS;
constexpr int kActivePercent = 100;
constexpr int kDimPercent = 15;

esp_panel::drivers::Backlight* backlightControl = nullptr;
bool dimmed = false;

} // namespace

void screenPowerBegin(esp_panel::drivers::Backlight* backlight)
{
    backlightControl = backlight;
    dimmed = false;
    if (backlightControl == nullptr) {
        Serial.println("Screen power: no backlight control on this board; dimming disabled");
    }
}

void screenPowerWake()
{
    lv_disp_trig_activity(nullptr);
}

void screenPowerUpdate()
{
    if (backlightControl == nullptr) {
        return;
    }

    // LVGL resets this timer on every touch the input driver reports.
    const uint32_t idleMs = lv_disp_get_inactive_time(nullptr);

    if (!dimmed && idleMs >= kDimAfterMs) {
        backlightControl->setBrightness(kDimPercent);
        dimmed = true;
        Serial.printf("Screen power: dimmed after %lu s idle\n", static_cast<unsigned long>(idleMs / 1000));
    } else if (dimmed && idleMs < kDimAfterMs) {
        backlightControl->setBrightness(kActivePercent);
        dimmed = false;
        Serial.println("Screen power: restored on touch");
    }
}
