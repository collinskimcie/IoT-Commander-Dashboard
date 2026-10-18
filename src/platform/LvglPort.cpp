#include "platform/LvglPort.h"

#include <esp_log.h>
#include <lvgl.h>

namespace {

constexpr char kLogTag[] = "CommanderDisplay";
constexpr uint16_t kDrawBufferLines = 20;
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t displayDriver;
lv_indev_drv_t inputDriver;
lv_color_t pixels[LV_HOR_RES_MAX * kDrawBufferLines];
esp_panel::drivers::LCD* display = nullptr;
esp_panel::drivers::Touch* touchPanel = nullptr;

void flushDisplay(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colors)
{
    auto* lcd = static_cast<esp_panel::drivers::LCD*>(driver->user_data);
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    if (!lcd->drawBitmap(
            area->x1,
            area->y1,
            width,
            height,
            reinterpret_cast<const uint8_t*>(colors),
            -1)) {
        ESP_LOGE(kLogTag, "LCD transfer failed for area (%d,%d)-(%d,%d)", area->x1, area->y1, area->x2, area->y2);
    }
    lv_disp_flush_ready(driver);
}

void readTouch(lv_indev_drv_t* driver, lv_indev_data_t* data)
{
    auto* touch = static_cast<esp_panel::drivers::Touch*>(driver->user_data);
    esp_panel::drivers::TouchPoint point;

    data->state = LV_INDEV_STATE_RELEASED;
    if (touch->readRawData(1, 0, 0) && touch->getPoints(&point, 1) > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
    }
}

} // namespace

bool lvglPortBegin(esp_panel::drivers::LCD* lcd, esp_panel::drivers::Touch* touch)
{
    if (lcd == nullptr || touch == nullptr) {
        return false;
    }

    display = lcd;
    touchPanel = touch;
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, LV_HOR_RES_MAX * kDrawBufferLines);

    lv_disp_drv_init(&displayDriver);
    displayDriver.hor_res = lcd->getFrameWidth();
    displayDriver.ver_res = lcd->getFrameHeight();
    displayDriver.flush_cb = flushDisplay;
    displayDriver.draw_buf = &drawBuffer;
    displayDriver.user_data = display;
    if (lv_disp_drv_register(&displayDriver) == nullptr) {
        return false;
    }

    lv_indev_drv_init(&inputDriver);
    inputDriver.type = LV_INDEV_TYPE_POINTER;
    inputDriver.read_cb = readTouch;
    inputDriver.user_data = touchPanel;
    return lv_indev_drv_register(&inputDriver) != nullptr;
}

void lvglPortHandle()
{
    lv_timer_handler();
}
