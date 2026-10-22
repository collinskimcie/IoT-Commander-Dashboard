#ifndef LV_CONF_H
#define LV_CONF_H

#include <esp_timer.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1
#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 320

#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)
/* LVGL's heap lives in PSRAM so internal RAM stays free for Wi-Fi and TLS. */
#define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <esp_timer.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (esp_timer_get_time() / 1000ULL)

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_BAR 1
#define LV_USE_SWITCH 1
#define LV_USE_LIST 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_MSGBOX 1
#define LV_USE_ARC 1
#define LV_USE_SPINNER 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#endif
