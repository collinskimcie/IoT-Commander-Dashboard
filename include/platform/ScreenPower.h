#pragma once

namespace esp_panel::drivers {
class Backlight;
}

// Dims the backlight after a period without touch and restores it on the next touch.
void screenPowerBegin(esp_panel::drivers::Backlight* backlight);
void screenPowerUpdate();   // call from loop(), on the same task that runs LVGL
