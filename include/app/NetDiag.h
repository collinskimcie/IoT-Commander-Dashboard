#pragma once

#include <Arduino.h>

#include "app/Domain.h"

// On-demand network checks (DNS, TCP reach, CA expiry). They run in a short-lived
// task so neither the UI nor the main loop ever blocks on the network.
namespace NetDiag {

constexpr size_t kLineCount = 5;

enum class State : uint8_t { Idle, Running, Done };

struct Result {
    char mqttHost[64];
    uint16_t mqttPort;
    char backendHost[64];
    uint16_t backendPort;
    int dnsMs;
    char dnsIp[16];
    int mqttTcpMs;
    int backendTcpMs;
    int mqttCaStatus;
    int mqttCaDays;
    int backendCaStatus;
    int backendCaDays;
};

struct Line {
    bool ok;
    String text;
};

// False if a run is already in progress or Wi-Fi is down.
bool start(const MqttSettings& mqtt, const TerminalSettings& terminal, bool printToSerial);
State state();
Result result();   // meaningful once state() == Done
Line describe(const Result& result, size_t index);

} // namespace NetDiag
