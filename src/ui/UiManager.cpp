#include "ui/UiManager.h"

#include "ui/UiComponents.h"

namespace {

lv_color_t primaryColor()
{
    return lv_color_hex(0x18C29C);
}

lv_color_t backgroundColor(bool dark)
{
    return dark ? lv_color_hex(0x0B1220) : lv_color_hex(0xF2F5F7);
}

lv_color_t surfaceColor(bool dark)
{
    return dark ? lv_color_hex(0x172235) : lv_color_hex(0xFFFFFF);
}

} // namespace

UiManager::UiManager(
    ConnectivityService& connectivity,
    MqttService& mqtt,
    NotificationCenter& notifications,
    SettingsStore& settings,
    TimeService& time
) :
    _connectivity(connectivity),
    _mqtt(mqtt),
    _notifications(notifications),
    _settings(settings),
    _time(time),
    _context{
        connectivity,
        mqtt,
        notifications,
        settings,
        time,
        navigate,
        applyTheme,
        this
    }
{
}

bool UiManager::begin()
{
    if (lv_disp_get_default() == nullptr) {
        return false;
    }

    applyTheme(this);
    _root = lv_scr_act();
    lv_obj_set_style_bg_color(_root, backgroundColor(_settings.darkTheme()), 0);
    lv_obj_set_style_pad_all(_root, 0, 0);

    _header = lv_obj_create(_root);
    lv_obj_set_size(_header, LV_PCT(100), 48);
    lv_obj_set_pos(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_pad_hor(_header, 12, 0);
    lv_obj_set_style_pad_ver(_header, 6, 0);
    lv_obj_set_flex_flow(_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    _title = uiCreateLabel(_header, "OVERVIEW", 16);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_16, 0);
    _networkStatus = uiCreateLabel(_header, "OFFLINE", 12);
    lv_obj_set_style_text_color(_networkStatus, lv_color_hex(0xE6B566), 0);

    _content = lv_obj_create(_root);
    lv_obj_set_size(_content, LV_PCT(100), LV_VER_RES_MAX - 96);
    lv_obj_set_pos(_content, 0, 48);
    lv_obj_set_style_radius(_content, 0, 0);
    lv_obj_set_style_border_width(_content, 0, 0);
    lv_obj_set_style_pad_all(_content, 12, 0);
    lv_obj_set_style_pad_row(_content, 10, 0);
    lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_content, LV_SCROLLBAR_MODE_AUTO);

    _navigation = lv_obj_create(_root);
    lv_obj_set_size(_navigation, LV_PCT(100), 48);
    lv_obj_set_pos(_navigation, 0, LV_VER_RES_MAX - 48);
    lv_obj_set_style_radius(_navigation, 0, 0);
    lv_obj_set_style_border_width(_navigation, 0, 0);
    lv_obj_set_style_pad_all(_navigation, 2, 0);
    lv_obj_set_flex_flow(_navigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_navigation, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_navigation, LV_OBJ_FLAG_SCROLLABLE);

    const struct {
        AppPage page;
        const char* text;
    } entries[] = {
        {AppPage::Dashboard, LV_SYMBOL_HOME " Home"},
        {AppPage::Devices, LV_SYMBOL_LIST " Devices"},
        {AppPage::Alerts, LV_SYMBOL_BELL " Alerts"},
        {AppPage::Settings, LV_SYMBOL_SETTINGS " Setup"}
    };
    for (const auto& entry : entries) {
        lv_obj_t* button = lv_btn_create(_navigation);
        lv_obj_set_size(button, 55, 38);
        lv_obj_set_style_radius(button, 9, 0);
        const uintptr_t pageData = static_cast<uintptr_t>(entry.page) + 1;
        lv_obj_add_event_cb(button, onNavigation, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(pageData));
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, entry.text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_center(label);
        if (entry.page == AppPage::Alerts) {
            _unreadBadge = lv_label_create(button);
            lv_label_set_text(_unreadBadge, "");
            lv_obj_set_style_text_color(_unreadBadge, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_color(_unreadBadge, lv_color_hex(0xD84A4A), 0);
            lv_obj_set_style_bg_opa(_unreadBadge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(_unreadBadge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_hor(_unreadBadge, 4, 0);
            lv_obj_align(_unreadBadge, LV_ALIGN_TOP_RIGHT, 2, -4);
        }
    }

    show(AppPage::Dashboard);
    return true;
}

void UiManager::update()
{
    _connectivity.update();
    _mqtt.update(_settings.mqttSettings());
    _time.update(_settings.terminalSettings().timezone, _connectivity.wifiConnected());
    if (millis() - _lastUiRefresh < 100) {
        return;
    }
    _lastUiRefresh = millis();
    lv_label_set_text(_networkStatus, _connectivity.wifiConnected() ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(
        _networkStatus,
        _connectivity.wifiConnected() ? lv_color_hex(0x65D6A5) : lv_color_hex(0xE6B566),
        0);
    lv_label_set_text_fmt(_unreadBadge, "%u", static_cast<unsigned>(_notifications.unreadCount()));
    lv_obj_add_flag(_unreadBadge, _notifications.unreadCount() == 0 ? LV_OBJ_FLAG_HIDDEN : 0);
    moduleFor(_currentPage).update(_context);
}

void UiManager::show(AppPage page)
{
    if (_currentPage != AppPage::Alerts && page == AppPage::Alerts) {
        _notifications.markAllRead();
    }
    _currentPage = page;
    lv_label_set_text(_title, moduleFor(page).title);
    rebuildContent();
}

void UiManager::onNavigation(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    auto* self = static_cast<UiManager*>(lv_event_get_user_data(event));
    const auto pageIndex = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(event)));
    if (pageIndex == 0) {
        return;
    }
    const auto page = static_cast<AppPage>(pageIndex - 1);
    self->show(page);
}

void UiManager::navigate(void* owner, AppPage page)
{
    static_cast<UiManager*>(owner)->show(page);
}

void UiManager::applyTheme(void* owner)
{
    auto* self = static_cast<UiManager*>(owner);
    lv_theme_t* theme = lv_theme_default_init(
        lv_disp_get_default(),
        primaryColor(),
        lv_color_hex(0x546E7A),
        self->_settings.darkTheme(),
        &lv_font_montserrat_14);
    lv_disp_set_theme(lv_disp_get_default(), theme);
    if (self->_root != nullptr) {
        const bool dark = self->_settings.darkTheme();
        lv_obj_set_style_bg_color(self->_root, backgroundColor(dark), 0);
        lv_obj_set_style_bg_color(self->_header, surfaceColor(dark), 0);
        lv_obj_set_style_bg_color(self->_navigation, surfaceColor(dark), 0);
        lv_obj_set_style_bg_opa(self->_content, LV_OPA_TRANSP, 0);
    }
}

const ScreenModule& UiManager::moduleFor(AppPage page) const
{
    switch (page) {
    case AppPage::Dashboard:
        return dashboardScreen();
    case AppPage::Devices:
        return devicesScreen();
    case AppPage::Alerts:
        return alertsScreen();
    case AppPage::Settings:
        return settingsScreen();
    }
    return dashboardScreen();
}

void UiManager::rebuildContent()
{
    const ScreenModule& module = moduleFor(_currentPage);
    module.destroy();
    lv_obj_clean(_content);
    module.create(_content, _context);
}
