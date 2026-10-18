#include "ui/ScreenModule.h"

#include "ui/UiComponents.h"

#include <cstring>

namespace {

ScreenContext* screenContext = nullptr;
lv_obj_t* timeLabel = nullptr;
lv_obj_t* connectionLabel = nullptr;
lv_obj_t* diagnosticsLabel = nullptr;
lv_obj_t* savedCountLabel = nullptr;
lv_obj_t* savedList = nullptr;
lv_obj_t* mqttStatusLabel = nullptr;
lv_obj_t* mqttReadingTitle = nullptr;
lv_obj_t* mqttReadingLabel = nullptr;
lv_obj_t* mqttReadingCard = nullptr;
lv_obj_t* mqttCommandRow = nullptr;
lv_obj_t* extraReadingRows[kMaxTelemetryReadings - 1] = {};
lv_obj_t* extraReadingTitles[kMaxTelemetryReadings - 1] = {};
lv_obj_t* extraReadingLabels[kMaxTelemetryReadings - 1] = {};
lv_obj_t* commandDialog = nullptr;
bool confirmedCommandEnabled = false;
uint32_t lastDiagnosticsUpdate = UINT32_MAX;

void onMqttCommandConfirm(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || commandDialog == nullptr) {
        return;
    }
    const char* selected = lv_msgbox_get_active_btn_text(commandDialog);
    if (selected != nullptr && std::strcmp(selected, "Send") == 0 && screenContext != nullptr) {
        if (screenContext->mqtt.publishCommand(confirmedCommandEnabled)) {
            screenContext->notifications.push(
                "MQTT command sent",
                confirmedCommandEnabled ? "Published ON." : "Published OFF.");
        } else {
            screenContext->notifications.push("MQTT command failed", "Check the broker and command topic.");
        }
    }
    lv_msgbox_close(commandDialog);
    commandDialog = nullptr;
}

void onMqttCommand(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr || commandDialog != nullptr) {
        return;
    }
    confirmedCommandEnabled = lv_event_get_user_data(event) != nullptr;
    static const char* buttons[] = {"Cancel", "Send", ""};
    commandDialog = lv_msgbox_create(
        lv_layer_top(),
        "Confirm command",
        confirmedCommandEnabled ? "Send ON to the connected system?" : "Send OFF to the connected system?",
        buttons,
        true);
    lv_obj_center(commandDialog);
    lv_obj_add_event_cb(commandDialog, onMqttCommandConfirm, LV_EVENT_VALUE_CHANGED, nullptr);
}

bool fieldIsSelected(const String& fields, const String& name)
{
    if (fields.isEmpty()) {
        return true;
    }
    size_t start = 0;
    while (start < fields.length()) {
        const int end = fields.indexOf(',', start);
        const String field = fields.substring(start, end < 0 ? fields.length() : end);
        if (field == name) {
            return true;
        }
        if (end < 0) {
            break;
        }
        start = static_cast<size_t>(end) + 1;
    }
    return false;
}

void onDiscover(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && screenContext != nullptr) {
        screenContext->navigate(screenContext->owner, AppPage::Devices);
    }
}

void renderSavedDevices()
{
    lv_obj_clean(savedList);
    const auto& devices = screenContext->settings.devices();
    if (devices.empty()) {
        lv_obj_t* empty = uiCreateLabel(savedList, "No systems saved yet", 14);
        uiSetMutedText(empty);
        return;
    }

    for (const SavedDevice& device : devices) {
        lv_obj_t* row = lv_obj_create(savedList);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        const char* transport = device.transport == Transport::Wifi ? "Wi-Fi" : "BLE";
        lv_obj_t* label = uiCreateLabel(row, device.name.c_str(), 14);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 135);
        lv_obj_t* type = uiCreateLabel(row, transport, 12);
        uiSetMutedText(type);
    }
}

lv_obj_t* createDashboard(lv_obj_t* parent, ScreenContext& context)
{
    screenContext = &context;
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = uiCreateLabel(page, "YOUR CONNECTED SYSTEMS", 12);
    uiSetMutedText(title);

    lv_obj_t* clock = uiCreateCard(page);
    uiCreateLabel(clock, "LOCAL TIME", 12);
    timeLabel = uiCreateLabel(clock, context.time.localTime().c_str(), 20);

    lv_obj_t* connection = uiCreateCard(page);
    uiCreateLabel(connection, "NETWORK", 12);
    connectionLabel = uiCreateLabel(connection, "Offline", 20);

    lv_obj_t* diagnostics = uiCreateCard(page);
    uiCreateLabel(diagnostics, "DEVICE DIAGNOSTICS", 12);
    diagnosticsLabel = uiCreateLabel(diagnostics, "", 12);

    lv_obj_t* mqtt = uiCreateCard(page);
    uiCreateLabel(mqtt, "MQTT BROKER", 12);
    mqttStatusLabel = uiCreateLabel(mqtt, context.mqtt.status().c_str(), 12);
    lv_label_set_long_mode(mqttStatusLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mqttStatusLabel, LV_PCT(100));
    mqttReadingCard = uiCreateCard(mqtt);
    lv_obj_set_style_pad_all(mqttReadingCard, 6, 0);
    mqttReadingTitle = uiCreateLabel(mqttReadingCard, context.settings.mqttSettings().tileLabel.c_str(), 12);
    mqttReadingLabel = uiCreateLabel(mqttReadingCard, "Waiting for reading", 20);
    for (size_t index = 0; index < kMaxTelemetryReadings - 1; ++index) {
        lv_obj_t* row = lv_obj_create(mqttReadingCard);
        extraReadingRows[index] = row;
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        extraReadingTitles[index] = uiCreateLabel(row, "", 12);
        lv_obj_set_width(extraReadingTitles[index], 135);
        extraReadingLabels[index] = uiCreateLabel(row, "", 14);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    mqttCommandRow = lv_obj_create(mqtt);
    lv_obj_set_width(mqttCommandRow, LV_PCT(100));
    lv_obj_set_height(mqttCommandRow, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(mqttCommandRow, 0, 0);
    lv_obj_set_style_border_width(mqttCommandRow, 0, 0);
    lv_obj_set_style_pad_column(mqttCommandRow, 8, 0);
    lv_obj_set_flex_flow(mqttCommandRow, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(mqttCommandRow, LV_OBJ_FLAG_SCROLLABLE);
    if (!context.settings.mqttSettings().commandTopic.isEmpty()) {
        uiCreateButton(mqttCommandRow, "ON", onMqttCommand, reinterpret_cast<void*>(1));
        uiCreateButton(mqttCommandRow, "OFF", onMqttCommand, nullptr);
    } else {
        lv_obj_add_flag(mqttCommandRow, LV_OBJ_FLAG_HIDDEN);
    }
    if (!context.settings.mqttSettings().tileEnabled) {
        lv_obj_add_flag(mqttReadingCard, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* devices = uiCreateCard(page);
    lv_obj_t* countRow = lv_obj_create(devices);
    lv_obj_set_width(countRow, LV_PCT(100));
    lv_obj_set_height(countRow, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(countRow, 0, 0);
    lv_obj_set_style_border_width(countRow, 0, 0);
    lv_obj_set_flex_flow(countRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(countRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(countRow, LV_OBJ_FLAG_SCROLLABLE);
    uiCreateLabel(countRow, "SAVED SYSTEMS", 12);
    savedCountLabel = uiCreateLabel(countRow, "0", 16);
    savedList = lv_obj_create(devices);
    lv_obj_set_width(savedList, LV_PCT(100));
    lv_obj_set_height(savedList, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(savedList, 0, 0);
    lv_obj_set_style_pad_row(savedList, 6, 0);
    lv_obj_set_style_border_width(savedList, 0, 0);
    lv_obj_set_flex_flow(savedList, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(savedList, LV_OBJ_FLAG_SCROLLABLE);
    renderSavedDevices();

    uiCreateButton(page, "Discover nearby systems", onDiscover, nullptr);
    return page;
}

void updateDashboard(ScreenContext& context)
{
    if (timeLabel != nullptr) {
        const String localTime = context.time.localTime();
        lv_label_set_text(timeLabel, localTime.c_str());
        lv_obj_set_style_text_color(
            timeLabel,
            context.time.synchronized() ? lv_color_hex(0x65D6A5) : lv_color_hex(0xE6B566),
            0);
    }
    if (connectionLabel != nullptr) {
        if (context.connectivity.wifiConnected()) {
            const String connected = context.connectivity.wifiSsid() + "\n" + context.connectivity.ipAddress();
            lv_label_set_text(connectionLabel, connected.c_str());
        } else {
            lv_label_set_text(connectionLabel, "Offline");
        }
    }
    const uint32_t uptimeSeconds = millis() / 1000UL;
    if (diagnosticsLabel != nullptr && uptimeSeconds != lastDiagnosticsUpdate) {
        lastDiagnosticsUpdate = uptimeSeconds;
        const uint32_t days = uptimeSeconds / 86400UL;
        const uint32_t hours = (uptimeSeconds / 3600UL) % 24UL;
        const uint32_t minutes = (uptimeSeconds / 60UL) % 60UL;
        const uint32_t seconds = uptimeSeconds % 60UL;
        String diagnostics = "Uptime: ";
        if (days > 0) {
            diagnostics += String(days) + "d ";
        }
        diagnostics += String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s";
        if (context.connectivity.wifiConnected()) {
            diagnostics += "\nWi-Fi signal: " + String(context.connectivity.wifiRssi()) + " dBm";
            diagnostics += "\nIP address: " + context.connectivity.ipAddress();
        } else {
            diagnostics += "\nWi-Fi signal: Offline\nIP address: Offline";
        }
        diagnostics += "\nFree heap: " + String(ESP.getFreeHeap() / 1024U) + " KB";
        lv_label_set_text(diagnosticsLabel, diagnostics.c_str());
    }
    if (savedCountLabel != nullptr) {
        lv_label_set_text_fmt(savedCountLabel, "%u", static_cast<unsigned>(context.settings.devices().size()));
    }
    if (mqttStatusLabel != nullptr) {
        const String status = context.mqtt.status();
        lv_label_set_text(mqttStatusLabel, status.c_str());
        lv_obj_set_style_text_color(
            mqttStatusLabel,
            context.mqtt.connected() ? lv_color_hex(0x65D6A5) : lv_color_hex(0xE6B566),
            0);
    }
    if (mqttReadingCard != nullptr) {
        const MqttSettings& config = context.settings.mqttSettings();
        if (config.tileEnabled) {
            lv_obj_clear_flag(mqttReadingCard, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(mqttReadingCard, LV_OBJ_FLAG_HIDDEN);
        }
        if (config.commandTopic.isEmpty()) {
            lv_obj_add_flag(mqttCommandRow, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(mqttCommandRow, LV_OBJ_FLAG_HIDDEN);
        }
        size_t shown = 0;
        for (const TelemetryReading& reading : context.mqtt.readings()) {
            if (!reading.valid || !fieldIsSelected(config.visibleFields, reading.name)) {
                continue;
            }
            const String value = String(reading.value, 2) +
                (config.unit.isEmpty() ? "" : " " + config.unit);
            if (shown == 0) {
                lv_label_set_text(mqttReadingTitle, reading.name.c_str());
                lv_label_set_text(mqttReadingLabel, value.c_str());
            } else if (shown <= kMaxTelemetryReadings - 1) {
                const size_t index = shown - 1;
                lv_label_set_text(extraReadingTitles[index], reading.name.c_str());
                lv_label_set_text(extraReadingLabels[index], value.c_str());
                lv_obj_clear_flag(extraReadingRows[index], LV_OBJ_FLAG_HIDDEN);
            }
            ++shown;
        }
        if (shown == 0) {
            lv_label_set_text(mqttReadingTitle, config.tileLabel.c_str());
            lv_label_set_text(mqttReadingLabel, "Waiting for reading");
        }
        for (size_t index = shown > 0 ? shown - 1 : 0; index < kMaxTelemetryReadings - 1; ++index) {
            lv_obj_add_flag(extraReadingRows[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void destroyDashboard()
{
    screenContext = nullptr;
    timeLabel = nullptr;
    connectionLabel = nullptr;
    diagnosticsLabel = nullptr;
    lastDiagnosticsUpdate = UINT32_MAX;
    savedCountLabel = nullptr;
    savedList = nullptr;
    mqttStatusLabel = nullptr;
    mqttReadingTitle = nullptr;
    mqttReadingLabel = nullptr;
    mqttReadingCard = nullptr;
    mqttCommandRow = nullptr;
    commandDialog = nullptr;
    for (size_t index = 0; index < kMaxTelemetryReadings - 1; ++index) {
        extraReadingRows[index] = nullptr;
        extraReadingTitles[index] = nullptr;
        extraReadingLabels[index] = nullptr;
    }
}

} // namespace

const ScreenModule& dashboardScreen()
{
    static const ScreenModule module{AppPage::Dashboard, "OVERVIEW", createDashboard, updateDashboard, destroyDashboard};
    return module;
}
