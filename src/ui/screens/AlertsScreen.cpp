#include "ui/ScreenModule.h"

#include "ui/UiComponents.h"

namespace {

ScreenContext* screenContext = nullptr;
lv_obj_t* notificationList = nullptr;
uint32_t renderedNotificationRevision = UINT32_MAX;

void renderNotifications()
{
    if (notificationList == nullptr || screenContext == nullptr) {
        return;
    }
    lv_obj_clean(notificationList);
    const auto& notifications = screenContext->notifications.items();
    if (notifications.empty()) {
        uiCreateLabel(notificationList, "You're all caught up.", 14);
        renderedNotificationRevision = screenContext->notifications.revision();
        return;
    }
    for (const Notification& notification : notifications) {
        lv_obj_t* card = uiCreateCard(notificationList);
        uiCreateLabel(card, notification.title.c_str(), 16);
        lv_obj_t* message = uiCreateLabel(card, notification.message.c_str(), 14);
        lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(message, LV_PCT(100));
    }
    renderedNotificationRevision = screenContext->notifications.revision();
}

void onClearNotifications(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && screenContext != nullptr) {
        screenContext->notifications.clear();
        renderNotifications();
    }
}

lv_obj_t* createAlerts(lv_obj_t* parent, ScreenContext& context)
{
    screenContext = &context;
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* heading = lv_obj_create(page);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_obj_set_height(heading, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(heading, 0, 0);
    lv_obj_set_style_border_width(heading, 0, 0);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(heading, LV_OBJ_FLAG_SCROLLABLE);
    uiCreateLabel(heading, "NOTIFICATION CENTER", 12);
    uiCreateButton(heading, "Clear", onClearNotifications, nullptr);

    notificationList = lv_obj_create(page);
    lv_obj_set_width(notificationList, LV_PCT(100));
    lv_obj_set_height(notificationList, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(notificationList, 0, 0);
    lv_obj_set_style_pad_row(notificationList, 8, 0);
    lv_obj_set_style_border_width(notificationList, 0, 0);
    lv_obj_set_flex_flow(notificationList, LV_FLEX_FLOW_COLUMN);

    renderNotifications();
    return page;
}

void updateAlerts(ScreenContext& context)
{
    if (renderedNotificationRevision != context.notifications.revision()) {
        renderNotifications();
    }
}

void destroyAlerts()
{
    screenContext = nullptr;
    notificationList = nullptr;
    renderedNotificationRevision = UINT32_MAX;
}

} // namespace

const ScreenModule& alertsScreen()
{
    static const ScreenModule module{AppPage::Alerts, "ALERTS", createAlerts, updateAlerts, destroyAlerts};
    return module;
}
