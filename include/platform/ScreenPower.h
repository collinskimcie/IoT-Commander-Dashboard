#pragma once

namespace esp_panel::drivers {
class Backlight;
}

// Dims the backlight after a period without touch and restores it on the next touch.
void screenPowerBegin(esp_panel::drivers::Backlight* backlight);
void screenPowerWake();   // counts as activity: restores the backlight on the next update
void screenPowerUpdate();   // call from loop(), on the same task that runs LVGL
