#include "ui/ScreenModule.h"

#include "app/BackendService.h"
#include "ui/UiComponents.h"

#include <WiFi.h>

namespace {

constexpr uint32_t kPassColor = 0x2E9E5B;
constexpr uint32_t kFailColor = 0xD84A4A;
constexpr size_t kCheckCount = 6;

lv_obj_t* checkLabels[kCheckCount] = {};
lv_obj_t* readingLabels[kMaxTelemetryReadings] = {};
lv_obj_t* signalBar = nullptr;
lv_obj_t* signalLabel = nullptr;
uint32_t lastRefreshAt = 0;

const char* backendText(BackendStatus status)
{
    switch (status) {
    case BackendStatus::NotConfigured:
        return "not configured";
    case BackendStatus::Connecting:
        return "connecting";
    case BackendStatus::Ok:
        return "ok";
    case BackendStatus::Retrying:
        return "retrying";
    case BackendStatus::AuthFailed:
        return "auth failed";
    }
    return "unknown";
}

void setCheck(size_t index, bool ok, const String& text)
{
    lv_obj_t* label = checkLabels[index];
    if (label == nullptr) {
        return;
    }
    lv_label_set_text_fmt(label, "%s %s", ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, text.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(ok ? kPassColor : kFailColor), 0);
}

void refresh(ScreenContext& context)
{
    const bool wifi = context.connectivity.wifiConnected();
    setCheck(0, wifi,
        wifi ? "Wi-Fi " + context.connectivity.wifiSsid() + "  " + context.connectivity.ipAddress()
             : String("Wi-Fi not connected"));

    const bool mqttUp = context.mqtt.connected();
    setCheck(1, mqttUp, mqttUp ? String("MQTT connected") : "MQTT " + context.mqtt.status());

    const BackendStatus backend = backendStatus();
    setCheck(2, backend == BackendStatus::Ok, String("Backend ") + backendText(backend));

    const bool timeSynced = context.time.synchronized();
    setCheck(3, timeSynced,
        timeSynced ? "Time " + context.time.localTime() : String("Time not synchronized"));

    const uint32_t heapFree = ESP.getFreeHeap();
    const uint32_t heapBlock = ESP.getMaxAllocHeap();
    setCheck(4, heapFree > 60000 && heapBlock > 30000,
        "Heap " + String(heapFree / 1024) + " KB, block " + String(heapBlock / 1024) + " KB");

    const uint32_t psramFree = ESP.getFreePsram();
    setCheck(5, psramFree > 1000000, "PSRAM " + String(psramFree / 1024) + " KB free");

    if (signalBar != nullptr && signalLabel != nullptr) {
        if (wifi) {
            const int rssi = WiFi.RSSI();
            int percent = (rssi + 90) * 100 / 60;
            percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
            lv_bar_set_value(signalBar, percent, LV_ANIM_OFF);
            lv_label_set_text_fmt(signalLabel, "%d dBm   channel %d", rssi, static_cast<int>(WiFi.channel()));
        } else {
            lv_bar_set_value(signalBar, 0, LV_ANIM_OFF);
            lv_label_set_text(signalLabel, "No Wi-Fi signal");
        }
    }

    const auto readings = context.mqtt.readings();
    size_t shown = 0;
    for (const TelemetryReading& reading : readings) {
        if (!reading.valid || shown >= kMaxTelemetryReadings || readingLabels[shown] == nullptr) {
            continue;
        }
        lv_label_set_text_fmt(readingLabels[shown], "%s = %.2f", reading.name.c_str(), static_cast<double>(reading.value));
        lv_obj_clear_flag(readingLabels[shown], LV_OBJ_FLAG_HIDDEN);
        ++shown;
    }
    if (shown == 0 && readingLabels[0] != nullptr) {
        lv_label_set_text(readingLabels[0], "No readings received yet");
        lv_obj_clear_flag(readingLabels[0], LV_OBJ_FLAG_HIDDEN);
        shown = 1;
    }
    for (size_t index = shown; index < kMaxTelemetryReadings; ++index) {
        if (readingLabels[index] != nullptr) {
            lv_obj_add_flag(readingLabels[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

lv_obj_t* createTools(lv_obj_t* parent, ScreenContext& context)
{
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* checks = uiCreateCard(page);
    uiCreateLabel(checks, "SELF-TEST", 12);
    for (size_t index = 0; index < kCheckCount; ++index) {
        checkLabels[index] = uiCreateLabel(checks, "...", 14);
        lv_label_set_long_mode(checkLabels[index], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(checkLabels[index], LV_PCT(100));
    }

    lv_obj_t* signal = uiCreateCard(page);
    uiCreateLabel(signal, "WI-FI SIGNAL", 12);
    signalBar = lv_bar_create(signal);
    lv_obj_set_size(signalBar, LV_PCT(100), 10);
    lv_bar_set_range(signalBar, 0, 100);
    signalLabel = uiCreateLabel(signal, "", 14);

    lv_obj_t* readings = uiCreateCard(page);
    uiCreateLabel(readings, "LIVE READINGS", 12);
    for (size_t index = 0; index < kMaxTelemetryReadings; ++index) {
        readingLabels[index] = uiCreateLabel(readings, "", 14);
        lv_obj_add_flag(readingLabels[index], LV_OBJ_FLAG_HIDDEN);
    }

    refresh(context);
    lastRefreshAt = millis();
    return page;
}

void updateTools(ScreenContext& context)
{
    if (millis() - lastRefreshAt < 1000) {
        return;
    }
    lastRefreshAt = millis();
    refresh(context);
}

void destroyTools()
{
    for (auto& label : checkLabels) {
        label = nullptr;
    }
    for (auto& label : readingLabels) {
        label = nullptr;
    }
    signalBar = nullptr;
    signalLabel = nullptr;
}

const ScreenModule module{AppPage::Tools, "Tools", createTools, updateTools, destroyTools};

} // namespace

const ScreenModule& toolsScreen()
{
    return module;
}
