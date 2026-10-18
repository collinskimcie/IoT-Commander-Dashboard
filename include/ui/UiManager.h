#pragma once

#include "ui/ScreenModule.h"

class UiManager {
public:
    UiManager(
        ConnectivityService& connectivity,
        MqttService& mqtt,
        NotificationCenter& notifications,
        SettingsStore& settings,
        TimeService& time);

    bool begin();
    void update();
    void show(AppPage page);

private:
    static void onNavigation(lv_event_t* event);
    static void navigate(void* owner, AppPage page);
    static void applyTheme(void* owner);
    const ScreenModule& moduleFor(AppPage page) const;
    void rebuildContent();

    ConnectivityService& _connectivity;
    MqttService& _mqtt;
    NotificationCenter& _notifications;
    SettingsStore& _settings;
    TimeService& _time;
    ScreenContext _context;
    AppPage _currentPage = AppPage::Dashboard;
    uint32_t _lastUiRefresh = 0;
    lv_obj_t* _root = nullptr;
    lv_obj_t* _header = nullptr;
    lv_obj_t* _content = nullptr;
    lv_obj_t* _navigation = nullptr;
    lv_obj_t* _title = nullptr;
    lv_obj_t* _networkStatus = nullptr;
    lv_obj_t* _unreadBadge = nullptr;
};
