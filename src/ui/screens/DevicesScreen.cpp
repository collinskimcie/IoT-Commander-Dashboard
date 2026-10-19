#include "ui/ScreenModule.h"

#include "ui/UiComponents.h"

namespace {

ScreenContext* screenContext = nullptr;
lv_obj_t* scanStatus = nullptr;
lv_obj_t* nearbyList = nullptr;
uint32_t renderedRevision = UINT32_MAX;
lv_obj_t* wifiOverlay = nullptr;
lv_obj_t* passwordField = nullptr;
String selectedSsid;

void renderNearby();

void onWifiScan(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && screenContext != nullptr) {
        screenContext->connectivity.startWifiScan();
    }
}

void onBluetoothScan(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && screenContext != nullptr) {
        screenContext->connectivity.startBluetoothScan();
    }
}

void closeWifiOverlay()
{
    if (wifiOverlay != nullptr) {
        lv_obj_del(wifiOverlay);
        wifiOverlay = nullptr;
        passwordField = nullptr;
    }
}

bool connectAndSaveWifi(const String& ssid, const String& password)
{
    if (screenContext == nullptr) {
        return false;
    }
    if (!screenContext->settings.setWifiCredentials(ssid, password)) {
        screenContext->notifications.push("Wi-Fi", "Could not save network credentials.");
        return false;
    }
    return screenContext->connectivity.connectWifi(ssid, password);
}

void onWifiKeyboard(lv_event_t* event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        closeWifiOverlay();
        return;
    }
    if (code != LV_EVENT_READY || screenContext == nullptr || passwordField == nullptr) {
        return;
    }

    const String password = lv_textarea_get_text(passwordField);
    if (connectAndSaveWifi(selectedSsid, password)) {
        screenContext->notifications.push("Wi-Fi", "Connecting to " + selectedSsid);
        closeWifiOverlay();
    }
}

void openWifiOverlay(const NearbyDevice& device)
{
    selectedSsid = device.id;
    if (!device.secured) {
        if (connectAndSaveWifi(device.id, "")) {
            screenContext->notifications.push("Wi-Fi", "Connecting to " + device.name);
        }
        return;
    }

    wifiOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wifiOverlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(wifiOverlay, 0, 0);
    lv_obj_set_style_bg_color(wifiOverlay, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(wifiOverlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(wifiOverlay, 0, 0);
    lv_obj_clear_flag(wifiOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* heading = uiCreateLabel(wifiOverlay, "Connect to Wi-Fi", 16);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t* network = uiCreateLabel(wifiOverlay, device.name.c_str(), 12);
    lv_obj_align_to(network, heading, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    passwordField = lv_textarea_create(wifiOverlay);
    lv_obj_set_size(passwordField, 216, 34);
    lv_obj_align(passwordField, LV_ALIGN_TOP_MID, 0, 58);
    lv_textarea_set_password_mode(passwordField, true);
    lv_textarea_set_one_line(passwordField, true);
    lv_textarea_set_placeholder_text(passwordField, "Network password");

    lv_obj_t* keyboard = lv_keyboard_create(wifiOverlay);
    lv_obj_set_size(keyboard, LV_PCT(100), 225);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(keyboard, passwordField);
    lv_obj_add_event_cb(keyboard, onWifiKeyboard, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, onWifiKeyboard, LV_EVENT_CANCEL, nullptr);
}

void onSaveDevice(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }
    const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const auto& devices = screenContext->connectivity.nearbyDevices();
    if (index == 0 || index > devices.size()) {
        return;
    }

    if (screenContext->settings.saveDevice(devices[index - 1])) {
        screenContext->notifications.push("System saved", devices[index - 1].name);
        renderNearby();
    } else {
        screenContext->notifications.push("Could not save system", "Storage is full or unavailable.");
    }
}

void onConnectDevice(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenContext == nullptr) {
        return;
    }
    const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const auto& devices = screenContext->connectivity.nearbyDevices();
    if (index == 0 || index > devices.size() || devices[index - 1].transport != Transport::Wifi) {
        return;
    }
    openWifiOverlay(devices[index - 1]);
}

void renderNearby()
{
    if (nearbyList == nullptr || screenContext == nullptr) {
        return;
    }

    lv_obj_clean(nearbyList);
    const auto& nearby = screenContext->connectivity.nearbyDevices();
    if (nearby.empty()) {
        lv_obj_t* empty = uiCreateLabel(nearbyList, "No systems found. Start a scan.", 14);
        uiSetMutedText(empty);
        return;
    }

    const auto& saved = screenContext->settings.devices();
    for (size_t index = 0; index < nearby.size(); ++index) {
        const NearbyDevice& device = nearby[index];
        lv_obj_t* row = uiCreateCard(nearbyList);
        lv_obj_t* name = uiCreateLabel(row, device.name.c_str(), 14);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        const String detail = String(device.transport == Transport::Wifi ? "Wi-Fi  " : "Bluetooth  ") +
            String(device.rssi) + " dBm" + (device.secured ? "  Locked" : "");
        lv_obj_t* info = uiCreateLabel(row, detail.c_str(), 12);
        uiSetMutedText(info);

        lv_obj_t* actions = lv_obj_create(row);
        lv_obj_set_width(actions, LV_PCT(100));
        lv_obj_set_height(actions, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(actions, 0, 0);
        lv_obj_set_style_border_width(actions, 0, 0);
        lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(actions, 8, 0);
        lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

        bool isSaved = false;
        for (const SavedDevice& savedDevice : saved) {
            if (savedDevice.id == device.id && savedDevice.transport == device.transport) {
                isSaved = true;
                break;
            }
        }
        const void* item = reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1));
        uiCreateButton(actions, isSaved ? "Saved" : "Save", onSaveDevice, const_cast<void*>(item));
        if (device.transport == Transport::Wifi) {
            uiCreateButton(actions, "Connect", onConnectDevice, const_cast<void*>(item));
        }
    }
}

lv_obj_t* createDevices(lv_obj_t* parent, ScreenContext& context)
{
    screenContext = &context;
    renderedRevision = UINT32_MAX;

    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    uiCreateLabel(page, "DISCOVER & CONFIGURE", 12);
    lv_obj_t* scanButtons = lv_obj_create(page);
    lv_obj_set_width(scanButtons, LV_PCT(100));
    lv_obj_set_height(scanButtons, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(scanButtons, 0, 0);
    lv_obj_set_style_border_width(scanButtons, 0, 0);
    lv_obj_set_flex_flow(scanButtons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scanButtons, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(scanButtons, LV_OBJ_FLAG_SCROLLABLE);
    uiCreateButton(scanButtons, "Scan Wi-Fi", onWifiScan, nullptr);
    uiCreateButton(scanButtons, "Scan BLE", onBluetoothScan, nullptr);

    scanStatus = uiCreateLabel(page, "Ready to discover systems", 12);
    uiSetMutedText(scanStatus);
    uiCreateLabel(page, "NEARBY SYSTEMS", 12);
    nearbyList = lv_obj_create(page);
    lv_obj_set_width(nearbyList, LV_PCT(100));
    lv_obj_set_height(nearbyList, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(nearbyList, 0, 0);
    lv_obj_set_style_pad_row(nearbyList, 8, 0);
    lv_obj_set_style_border_width(nearbyList, 0, 0);
    lv_obj_set_flex_flow(nearbyList, LV_FLEX_FLOW_COLUMN);

    renderedRevision = context.connectivity.scanRevision();
    renderNearby();
    return page;
}

void updateDevices(ScreenContext& context)
{
    if (scanStatus != nullptr) {
        const String status = context.connectivity.statusMessage();
        lv_label_set_text(scanStatus, status.c_str());
    }
    if (renderedRevision != context.connectivity.scanRevision()) {
        renderedRevision = context.connectivity.scanRevision();
        renderNearby();
    }
}

void destroyDevices()
{
    closeWifiOverlay();
    screenContext = nullptr;
    scanStatus = nullptr;
    nearbyList = nullptr;
    renderedRevision = UINT32_MAX;
}

} // namespace

const ScreenModule& devicesScreen()
{
    static const ScreenModule module{AppPage::Devices, "DEVICES", createDevices, updateDevices, destroyDevices};
    return module;
}
