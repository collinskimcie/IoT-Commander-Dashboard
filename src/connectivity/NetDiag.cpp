#include "app/NetDiag.h"

#include "app/ServerLocator.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <ctime>

namespace NetDiag {
namespace {

constexpr int kNone = -2;     // nothing to measure (not configured, or the host is an IP address)
constexpr int kFailed = -1;
constexpr uint32_t kConnectTimeoutMs = 3000;

enum CaStatus { kCaNone = 0, kCaOk = 1, kCaUnreadable = 2, kCaNoClock = 3 };

std::atomic<State> gState{State::Idle};
Result gRequest{};
Result gResult{};
String gMqttCa;
String gBackendCa;
bool gPrint = false;

int daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned shifted = (month + 9) % 12;
    const unsigned dayOfYear = (153 * shifted + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int>(dayOfEra) - 719468;
}

void caCheck(const String& pem, int& status, int& days)
{
    status = kCaNone;
    days = 0;
    if (pem.length() < 40) {
        return;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    const int rc = mbedtls_x509_crt_parse(
        &crt, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.length() + 1);
    if (rc != 0) {
        status = kCaUnreadable;
    } else {
        const time_t now = time(nullptr);
        if (now < 1700000000) {
            status = kCaNoClock;
        } else {
            status = kCaOk;
            days = daysFromCivil(crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day)
                - static_cast<int>(now / 86400);
        }
    }
    mbedtls_x509_crt_free(&crt);
}

int tcpConnectMs(const char* host, uint16_t port)
{
    if (host[0] == '\0' || port == 0) {
        return kNone;
    }
    WiFiClient client;
    const uint32_t started = millis();
    const bool ok = client.connect(host, port, kConnectTimeoutMs);
    const int elapsed = ok ? static_cast<int>(millis() - started) : kFailed;
    client.stop();
    return elapsed;
}

void parseUrl(const String& url, char* host, size_t hostSize, uint16_t& port)
{
    const String name = ServerLocator::hostOf(url);
    snprintf(host, hostSize, "%s", name.c_str());
    port = url.startsWith("https://") ? 443 : 80;
    const int scheme = url.indexOf("://");
    const int start = scheme < 0 ? 0 : scheme + 3;
    const int hostEnd = start + static_cast<int>(name.length());
    if (hostEnd < static_cast<int>(url.length()) && url[hostEnd] == ':') {
        const long parsed = url.substring(hostEnd + 1).toInt();
        if (parsed > 0 && parsed < 65536) {
            port = static_cast<uint16_t>(parsed);
        }
    }
}

Line describeCa(const char* label, int status, int days)
{
    const String name(label);
    switch (status) {
    case kCaNone:
        return {true, name + ": no certificate configured"};
    case kCaUnreadable:
        return {false, name + ": certificate could not be read"};
    case kCaNoClock:
        return {false, name + ": clock not synced, expiry unknown"};
    default:
        break;
    }
    if (days < 0) {
        return {false, name + " expired " + String(-days) + " days ago"};
    }
    if (days < 30) {
        return {false, name + " expires in " + String(days) + " days"};
    }
    return {true, name + ": valid, " + String(days) + " days left"};
}

Line describeTcp(const char* label, const char* host, uint16_t port, int ms)
{
    const String name(label);
    if (ms == kNone) {
        return {true, name + ": not configured"};
    }
    const String where = String(host) + ":" + String(port);
    if (ms == kFailed) {
        return {false, name + " " + where + " unreachable"};
    }
    return {true, name + " " + where + "  TCP " + String(ms) + " ms"};
}

void printResult(const Result& result)
{
    for (size_t index = 0; index < kLineCount; ++index) {
        const Line line = describe(result, index);
        Serial.printf("DIAG %s %s\n", line.ok ? "OK  " : "FAIL", line.text.c_str());
    }
}

void diagTask(void*)
{
    Result r = gRequest;

    IPAddress address;
    r.dnsIp[0] = '\0';
    if (r.mqttHost[0] == '\0' || address.fromString(r.mqttHost)) {
        r.dnsMs = kNone;
    } else {
        const uint32_t started = millis();
        if (WiFi.hostByName(r.mqttHost, address)) {
            r.dnsMs = static_cast<int>(millis() - started);
            snprintf(r.dnsIp, sizeof(r.dnsIp), "%s", address.toString().c_str());
        } else {
            r.dnsMs = kFailed;
        }
    }

    r.mqttTcpMs = tcpConnectMs(r.mqttHost, r.mqttPort);
    r.backendTcpMs = tcpConnectMs(r.backendHost, r.backendPort);
    caCheck(gMqttCa, r.mqttCaStatus, r.mqttCaDays);
    caCheck(gBackendCa, r.backendCaStatus, r.backendCaDays);

    gResult = r;
    gState.store(State::Done);
    if (gPrint) {
        printResult(r);
    }
    vTaskDelete(nullptr);
}

} // namespace

bool start(const MqttSettings& mqtt, const TerminalSettings& terminal, bool printToSerial)
{
    if (gState.load() == State::Running || WiFi.status() != WL_CONNECTED) {
        return false;
    }
    gRequest = Result{};
    snprintf(gRequest.mqttHost, sizeof(gRequest.mqttHost), "%s", mqtt.host.c_str());
    gRequest.mqttPort = mqtt.port;
    parseUrl(terminal.backendUrl, gRequest.backendHost, sizeof(gRequest.backendHost), gRequest.backendPort);
    gMqttCa = mqtt.rootCa;
    gBackendCa = terminal.backendRootCa;
    gPrint = printToSerial;
    gState.store(State::Running);
    if (xTaskCreate(diagTask, "net_diag", 6144, nullptr, 1, nullptr) != pdPASS) {
        gState.store(State::Idle);
        return false;
    }
    return true;
}

State state()
{
    return gState.load();
}

Result result()
{
    return gResult;
}

Line describe(const Result& result, size_t index)
{
    switch (index) {
    case 0:
        if (result.mqttHost[0] == '\0') {
            return {true, "DNS: MQTT host not configured"};
        }
        if (result.dnsMs == kNone) {
            return {true, String("DNS: ") + result.mqttHost + " is an IP address, no lookup"};
        }
        if (result.dnsMs == kFailed) {
            return {false, String("DNS: lookup of ") + result.mqttHost + " failed"};
        }
        return {true, String("DNS: ") + result.mqttHost + " -> " + result.dnsIp + "  " + String(result.dnsMs) + " ms"};
    case 1:
        return describeTcp("MQTT", result.mqttHost, result.mqttPort, result.mqttTcpMs);
    case 2:
        return describeTcp("Backend", result.backendHost, result.backendPort, result.backendTcpMs);
    case 3:
        return describeCa("MQTT CA", result.mqttCaStatus, result.mqttCaDays);
    default:
        return describeCa("Backend CA", result.backendCaStatus, result.backendCaDays);
    }
}

} // namespace NetDiag
