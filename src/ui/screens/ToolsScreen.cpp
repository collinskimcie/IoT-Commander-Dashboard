#include "ui/ScreenModule.h"

#include "app/BackendService.h"
#include "app/NetDiag.h"
#include "ui/UiComponents.h"

#include <WiFi.h>

namespace {

constexpr uint32_t kPassColor = 0x2E9E5B;
constexpr uint32_t kFailColor = 0xD84A4A;
constexpr size_t kCheckCount = 6;
constexpr size_t kTraceRows = 5;
constexpr uint8_t kDiagUnknown = 255;

ScreenContext* screenContext = nullptr;
lv_obj_t* checkLabels[kCheckCount] = {};
lv_obj_t* diagHint = nullptr;
lv_obj_t* diagLabels[NetDiag::kLineCount] = {};
lv_obj_t* traceSummary = nullptr;
lv_obj_t* traceTopic[kTraceRows] = {};
lv_obj_t* tracePayload[kTraceRows] = {};
lv_obj_t* readingLabels[kMaxTelemetryReadings] = {};
lv_obj_t* signalBar = nullptr;
lv_obj_t* signalLabel = nullptr;
uint32_t lastRefreshAt = 0;
uint8_t shownDiag = kDiagUnknown;
uint32_t rateAt = 0;
uint32_t rateTotal = 0;
uint32_t ratePerMinute = 0;

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

void setLine(lv_obj_t* label, bool ok, const String& text)
{
    if (label == nullptr) {
        return;
    }
    const String shown = String(ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE) + " " + text;
    lv_label_set_text(label, shown.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(ok ? kPassColor : kFailColor), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

void setCheck(size_t index, bool ok, const String& text)
{
    setLine(checkLabels[index], ok, text);
}

void onRunDiag(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }
    if (!NetDiag::start(screenContext->settings.mqttSettings(), screenContext->settings.terminalSettings(), false)) {
        screenContext->notifications.push("Diagnostics", "Could not start: already running or Wi-Fi is down.");
        return;
    }
    shownDiag = kDiagUnknown;
}

void refreshChecks(ScreenContext& context)
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

void refreshDiag()
{
    const NetDiag::State state = NetDiag::state();
    const uint8_t code = static_cast<uint8_t>(state);
    if (code == shownDiag || diagHint == nullptr) {
        return;
    }
    shownDiag = code;
    if (state == NetDiag::State::Done) {
        const NetDiag::Result result = NetDiag::result();
        lv_obj_add_flag(diagHint, LV_OBJ_FLAG_HIDDEN);
        for (size_t index = 0; index < NetDiag::kLineCount; ++index) {
            const NetDiag::Line line = NetDiag::describe(result, index);
            setLine(diagLabels[index], line.ok, line.text);
        }
        return;
    }
    for (lv_obj_t* label : diagLabels) {
        if (label != nullptr) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(diagHint,
        state == NetDiag::State::Running ? "Running..." : "Tap Run to test DNS, reachability and CA expiry.");
    lv_obj_clear_flag(diagHint, LV_OBJ_FLAG_HIDDEN);
}

void refreshTrace(ScreenContext& context)
{
    if (traceSummary == nullptr) {
        return;
    }
    const uint32_t now = millis();
    const uint32_t total = context.mqtt.messageTotal();
    if (now - rateAt >= 10000) {
        ratePerMinute = static_cast<uint32_t>(
            static_cast<uint64_t>(total - rateTotal) * 60000ULL / (now - rateAt));
        rateAt = now;
        rateTotal = total;
    }

    MqttService::MessageTrace rows[kTraceRows];
    const size_t count = context.mqtt.recentMessages(rows, kTraceRows);
    if (count == 0) {
        lv_label_set_text(traceSummary, "No MQTT messages received yet");
    } else {
        lv_label_set_text_fmt(traceSummary, "%u received, about %u per minute",
            static_cast<unsigned>(total), static_cast<unsigned>(ratePerMinute));
    }
    for (size_t index = 0; index < kTraceRows; ++index) {
        if (traceTopic[index] == nullptr || tracePayload[index] == nullptr) {
            continue;
        }
        if (index >= count) {
            lv_obj_add_flag(traceTopic[index], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tracePayload[index], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const unsigned age = static_cast<unsigned>((now - rows[index].receivedAt) / 1000U);
        lv_label_set_text_fmt(traceTopic[index], "%us ago  %s", age, rows[index].topic);
        lv_label_set_text_fmt(tracePayload[index], "%u B  %s",
            static_cast<unsigned>(rows[index].length), rows[index].text);
        lv_obj_clear_flag(traceTopic[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tracePayload[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void refresh(ScreenContext& context)
{
    refreshChecks(context);
    refreshDiag();
    refreshTrace(context);
}

lv_obj_t* wrapLabel(lv_obj_t* parent, const char* text, uint16_t size)
{
    lv_obj_t* label = uiCreateLabel(parent, text, size);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

lv_obj_t* createTools(lv_obj_t* parent, ScreenContext& context)
{
    screenContext = &context;
    shownDiag = kDiagUnknown;
    rateAt = millis();
    rateTotal = context.mqtt.messageTotal();

    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* checks = uiCreateCard(page);
    uiCreateLabel(checks, "SELF-TEST", 12);
    for (size_t index = 0; index < kCheckCount; ++index) {
        checkLabels[index] = wrapLabel(checks, "...", 14);
    }

    lv_obj_t* signal = uiCreateCard(page);
    uiCreateLabel(signal, "WI-FI SIGNAL", 12);
    signalBar = lv_bar_create(signal);
    lv_obj_set_size(signalBar, LV_PCT(100), 10);
    lv_bar_set_range(signalBar, 0, 100);
    signalLabel = uiCreateLabel(signal, "", 14);

    lv_obj_t* diag = uiCreateCard(page);
    uiCreateLabel(diag, "NETWORK DIAGNOSTICS", 12);
    uiCreateButton(diag, "Run diagnostics", onRunDiag, nullptr);
    diagHint = wrapLabel(diag, "", 14);
    for (size_t index = 0; index < NetDiag::kLineCount; ++index) {
        diagLabels[index] = wrapLabel(diag, "", 14);
        lv_obj_add_flag(diagLabels[index], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* trace = uiCreateCard(page);
    uiCreateLabel(trace, "MQTT INSPECTOR", 12);
    traceSummary = wrapLabel(trace, "", 14);
    for (size_t index = 0; index < kTraceRows; ++index) {
        traceTopic[index] = wrapLabel(trace, "", 12);
        tracePayload[index] = wrapLabel(trace, "", 12);
        uiSetMutedText(tracePayload[index]);
        lv_obj_add_flag(traceTopic[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tracePayload[index], LV_OBJ_FLAG_HIDDEN);
    }

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
    for (auto& label : diagLabels) {
        label = nullptr;
    }
    for (auto& label : traceTopic) {
        label = nullptr;
    }
    for (auto& label : tracePayload) {
        label = nullptr;
    }
    for (auto& label : readingLabels) {
        label = nullptr;
    }
    diagHint = nullptr;
    traceSummary = nullptr;
    signalBar = nullptr;
    signalLabel = nullptr;
    shownDiag = kDiagUnknown;
}

const ScreenModule module{AppPage::Tools, "Tools", createTools, updateTools, destroyTools};

} // namespace

const ScreenModule& toolsScreen()
{
    return module;
}
