#pragma once

#include "app/ConnectivityService.h"
#include "app/MqttService.h"
#include "app/NotificationCenter.h"
#include "app/SettingsStore.h"
#include "app/TimeService.h"

#include <lvgl.h>

enum class AppPage : uint8_t {
    Dashboard,
    Devices,
    Alerts,
    Settings
};

struct ScreenContext {
    ConnectivityService& connectivity;
    MqttService& mqtt;
    NotificationCenter& notifications;
    SettingsStore& settings;
    TimeService& time;
    void (*navigate)(void* owner, AppPage page);
    void (*applyTheme)(void* owner);
    void* owner;
};

struct ScreenModule {
    AppPage page;
    const char* title;
    lv_obj_t* (*create)(lv_obj_t* parent, ScreenContext& context);
    void (*update)(ScreenContext& context);
    void (*destroy)();
};

const ScreenModule& dashboardScreen();
const ScreenModule& devicesScreen();
const ScreenModule& alertsScreen();
const ScreenModule& settingsScreen();
