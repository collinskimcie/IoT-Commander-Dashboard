#include "ui/ScreenModule.h"

#include "ui/UiComponents.h"


#include <cmath>
#include <cstdlib>

namespace {

enum class InputField : uint8_t {
    Host,
    Port,
    Username,
    Password,
    TelemetryTopic,
    CommandTopic,
    TileLabel,
    Unit,
    Threshold,
    VisibleFields,
    RootCa,
    BackendUrl,
    BackendKey,
    BackendRootCa,
    Timezone,
    Count
};

ScreenContext* screenContext = nullptr;
lv_obj_t* themeLabel = nullptr;
lv_obj_t* savedCount = nullptr;
lv_obj_t* mqttStatus = nullptr;
lv_obj_t* keyboard = nullptr;
lv_obj_t* tileSwitch = nullptr;
lv_obj_t* alertSwitch = nullptr;
lv_obj_t* inputFields[static_cast<size_t>(InputField::Count)] = {};
MqttSettings editSettings;
TerminalSettings editTerminalSettings;

lv_obj_t* createInput(
    lv_obj_t* parent,
    const char* label,
    const String& value,
    InputField field,
    bool password = false,
    bool multiline = false,
    uint16_t maxLength = 96)
{
    uiCreateLabel(parent, label, 12);
    lv_obj_t* input = lv_textarea_create(parent);
    lv_obj_set_width(input, LV_PCT(100));
    lv_obj_set_height(input, multiline ? 86 : 32);
    lv_textarea_set_one_line(input, !multiline);
    // Set the text BEFORE max length / password mode. With either active, LVGL 8
    // inserts text one character at a time and re-lays out the page for each one,
    // which froze the UI for ~34 s when the field held a PEM certificate.
    lv_textarea_set_text(input, value.c_str());
    lv_textarea_set_max_length(input, maxLength);
    lv_textarea_set_password_mode(input, password);
    lv_obj_add_event_cb(input, [](lv_event_t* event) {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || keyboard == nullptr) {
            return;
        }
        lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, nullptr);
    inputFields[static_cast<size_t>(field)] = input;
    return input;
}

lv_obj_t* createSwitchRow(lv_obj_t* parent, const char* label, bool enabled)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 38);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    uiCreateLabel(row, label, 12);
    lv_obj_t* control = lv_switch_create(row);
    lv_obj_set_size(control, 42, 23);
    if (enabled) {
        lv_obj_add_state(control, LV_STATE_CHECKED);
    }
    return control;
}

void onKeyboard(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void onToggleTheme(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }
    screenContext->settings.setDarkTheme(!screenContext->settings.darkTheme());
    screenContext->applyTheme(screenContext->owner);
    lv_label_set_text(themeLabel, screenContext->settings.darkTheme() ? "Appearance: Dark" : "Appearance: Light");
}

bool readPort(uint16_t& port)
{
    const char* text = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Port)]);
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (text == end || *end != '\0' || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool readThreshold(float& threshold)
{
    const char* text = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Threshold)]);
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (text == end || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    threshold = value;
    return true;
}

void onSaveMqtt(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }
    if (!readPort(editSettings.port)) {
        screenContext->notifications.push("MQTT settings not saved", "Port must be between 1 and 65535.");
        return;
    }

    editSettings.host = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Host)]);
    editSettings.username = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Username)]);
    editSettings.password = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Password)]);
    editSettings.telemetryTopic = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::TelemetryTopic)]);
    editSettings.commandTopic = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::CommandTopic)]);
    editSettings.tileLabel = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::TileLabel)]);
    editSettings.unit = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Unit)]);
    editSettings.visibleFields = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::VisibleFields)]);
    editSettings.rootCa = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::RootCa)]);
    editSettings.alertEnabled = lv_obj_has_state(alertSwitch, LV_STATE_CHECKED);
    editSettings.tileEnabled = lv_obj_has_state(tileSwitch, LV_STATE_CHECKED);

    if (editSettings.alertEnabled && !readThreshold(editSettings.alertThreshold)) {
        screenContext->notifications.push("MQTT settings not saved", "Enter a numeric alert threshold.");
        return;
    }
    if (screenContext->settings.setMqttSettings(editSettings)) {
        screenContext->notifications.push("MQTT settings saved", "Broker configuration is stored on this terminal.");
        lv_label_set_text(mqttStatus, "Settings saved");
    } else {
        screenContext->notifications.push("MQTT settings not saved", "Check field lengths and local storage.");
    }
}

void onSaveBackend(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }

    editTerminalSettings.backendUrl = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::BackendUrl)]);
    editTerminalSettings.backendApiKey = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::BackendKey)]);
    editTerminalSettings.backendRootCa = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::BackendRootCa)]);
    editTerminalSettings.timezone = lv_textarea_get_text(inputFields[static_cast<size_t>(InputField::Timezone)]);
    if (!screenContext->settings.setTerminalSettings(editTerminalSettings)) {
        screenContext->notifications.push("Terminal settings not saved", "Check field lengths and local storage.");
        return;
    }
    screenContext->notifications.push("Terminal settings saved", "Backend and timezone settings are stored locally.");
}

void onCommand(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }
    const bool enabled = lv_event_get_user_data(event) != nullptr;
    if (screenContext->mqtt.publishCommand(enabled)) {
        screenContext->notifications.push("MQTT command sent", enabled ? "Published ON." : "Published OFF.");
    } else {
        screenContext->notifications.push("MQTT command failed", "Check the broker and command topic.");
    }
}

lv_obj_t* createSettings(lv_obj_t* parent, ScreenContext& context)
{
    screenContext = &context;
    editSettings = context.settings.mqttSettings();
    editTerminalSettings = context.settings.terminalSettings();
    for (lv_obj_t*& input : inputFields) {
        input = nullptr;
    }

    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    uiCreateLabel(page, "LOCAL CONFIGURATION", 12);
    lv_obj_t* appearance = uiCreateCard(page);
    uiCreateLabel(appearance, "Appearance", 16);
    themeLabel = uiCreateLabel(
        appearance,
        context.settings.darkTheme() ? "Appearance: Dark" : "Appearance: Light",
        14);
    uiCreateButton(appearance, "Toggle theme", onToggleTheme, nullptr);

    lv_obj_t* storage = uiCreateCard(page);
    uiCreateLabel(storage, "Saved systems", 16);
    savedCount = uiCreateLabel(storage, "", 14);

    lv_obj_t* mqtt = uiCreateCard(page);
    uiCreateLabel(mqtt, "MQTT BROKER", 16);
    mqttStatus = uiCreateLabel(mqtt, context.mqtt.status().c_str(), 12);
    createInput(mqtt, "Broker host / IP", editSettings.host, InputField::Host);
    createInput(mqtt, "Port", String(editSettings.port), InputField::Port);
    createInput(mqtt, "Username (optional)", editSettings.username, InputField::Username);
    createInput(mqtt, "Password (optional)", editSettings.password, InputField::Password, true);
    createInput(mqtt, "Numeric or JSON telemetry topic", editSettings.telemetryTopic, InputField::TelemetryTopic);
    createInput(mqtt, "Command topic (ON / OFF)", editSettings.commandTopic, InputField::CommandTopic);
    createInput(mqtt, "Dashboard tile name", editSettings.tileLabel, InputField::TileLabel);
    createInput(mqtt, "Reading unit", editSettings.unit, InputField::Unit);
    createInput(mqtt, "Dashboard JSON fields (comma-separated, blank = all)", editSettings.visibleFields, InputField::VisibleFields);
    createInput(mqtt, "Broker root CA certificate (PEM)", editSettings.rootCa, InputField::RootCa, false, true, 2048);
    createInput(
        mqtt,
        "Alert when reading reaches",
        String(editSettings.alertThreshold, 2),
        InputField::Threshold);
    tileSwitch = createSwitchRow(mqtt, "Show reading tile", editSettings.tileEnabled);
    alertSwitch = createSwitchRow(mqtt, "Enable threshold alert", editSettings.alertEnabled);
    uiCreateButton(mqtt, "Save MQTT settings", onSaveMqtt, nullptr);

    lv_obj_t* backend = uiCreateCard(page);
    uiCreateLabel(backend, "TIME AND BACKEND", 16);
    createInput(backend, "POSIX timezone (Kenya: EAT-3 = UTC+3)", editTerminalSettings.timezone, InputField::Timezone, false, false, 40);
    createInput(backend, "Backend API base URL", editTerminalSettings.backendUrl, InputField::BackendUrl, false, false, 128);
    createInput(backend, "Backend API key", editTerminalSettings.backendApiKey, InputField::BackendKey, true, false, 64);
    createInput(backend, "Backend root CA certificate (PEM, HTTPS only)", editTerminalSettings.backendRootCa, InputField::BackendRootCa, false, true, 2048);
    uiCreateButton(backend, "Save time and backend settings", onSaveBackend, nullptr);
    lv_obj_t* backendHint = uiCreateLabel(backend, "HTTP is only suitable for a trusted local network. Use HTTPS and a trusted CA outside development.", 11);
    lv_label_set_long_mode(backendHint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(backendHint, LV_PCT(100));

    keyboard = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(keyboard, LV_PCT(100), 142);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, onKeyboard, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, onKeyboard, LV_EVENT_CANCEL, nullptr);

    lv_obj_t* board = uiCreateCard(page);
    uiCreateLabel(board, "Platform", 16);
    uiCreateLabel(board, "Viewe UEDX24320028E-WB-A", 14);
    uiCreateLabel(board, "ESP32-S3  •  240 × 320  •  LVGL 8", 12);
    return page;
}

void updateSettings(ScreenContext& context)
{
    if (savedCount != nullptr) {
        lv_label_set_text_fmt(savedCount, "%u configured", static_cast<unsigned>(context.settings.devices().size()));
    }
    if (mqttStatus != nullptr) {
        const String status = context.mqtt.status();
        lv_label_set_text(mqttStatus, status.c_str());
    }
}

void destroySettings()
{
    if (keyboard != nullptr) {
        lv_obj_del(keyboard);
    }
    screenContext = nullptr;
    themeLabel = nullptr;
    savedCount = nullptr;
    mqttStatus = nullptr;
    keyboard = nullptr;
    tileSwitch = nullptr;
    alertSwitch = nullptr;
    for (lv_obj_t*& input : inputFields) {
        input = nullptr;
    }
}

} // namespace

const ScreenModule& settingsScreen()
{
    static const ScreenModule module{AppPage::Settings, "SETTINGS", createSettings, updateSettings, destroySettings};
    return module;
}
