#include "ui/UiComponents.h"

lv_obj_t* uiCreateCard(lv_obj_t* parent, int height)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    if (height == LV_SIZE_CONTENT) {
        lv_obj_set_height(card, LV_SIZE_CONTENT);
    } else {
        lv_obj_set_height(card, height);
    }
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t* uiCreateLabel(lv_obj_t* parent, const char* text, uint16_t fontSize)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    const lv_font_t* font = &lv_font_montserrat_14;
    if (fontSize >= 24) {
        font = &lv_font_montserrat_24;
    } else if (fontSize >= 20) {
        font = &lv_font_montserrat_20;
    } else if (fontSize >= 16) {
        font = &lv_font_montserrat_16;
    } else if (fontSize <= 12) {
        font = &lv_font_montserrat_12;
    }
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

lv_obj_t* uiCreateButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, void* userData)
{
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_height(button, 38);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, userData);
    }
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

void uiSetMutedText(lv_obj_t* label)
{
    lv_obj_set_style_text_opa(label, LV_OPA_70, 0);
}
