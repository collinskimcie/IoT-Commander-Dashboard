#pragma once

#include <esp_display_panel.hpp>

bool lvglPortBegin(esp_panel::drivers::LCD* lcd, esp_panel::drivers::Touch* touch);
void lvglPortHandle();
