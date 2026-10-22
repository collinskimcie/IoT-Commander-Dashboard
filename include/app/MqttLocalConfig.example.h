#pragma once

// TEMPLATE: copy to MqttLocalConfig.h (gitignored) and fill in your own values.
// Never commit real credentials.
namespace MqttLocalConfig {
static constexpr char host[] = "broker.example.com";
static constexpr unsigned int port = 1883;
static constexpr char username[] = "mqtt-user";
static constexpr char password[] = "change-me";
static constexpr char telemetryTopic[] = "sensors/#";
static constexpr char rootCa[] = R"IOTCA(-----BEGIN CERTIFICATE-----
PASTE YOUR BROKER ROOT CA (PEM) HERE
-----END CERTIFICATE-----
)IOTCA";
}
