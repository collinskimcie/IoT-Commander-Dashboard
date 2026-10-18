#pragma once

#include <lvgl.h>

lv_obj_t* uiCreateCard(lv_obj_t* parent, int height = LV_SIZE_CONTENT);
lv_obj_t* uiCreateLabel(lv_obj_t* parent, const char* text, uint16_t fontSize = 14);
lv_obj_t* uiCreateButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, void* userData);
void uiSetMutedText(lv_obj_t* label);
