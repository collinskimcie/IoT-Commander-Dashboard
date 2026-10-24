#!/usr/bin/env python3
"""IoT Commander device simulator: publishes fake telemetry over MQTT/TLS.

Credentials come from the MQTT_USER and MQTT_PASS environment variables,
never from the command line, so they stay out of shell history.
"""
import argparse
import json
import math
import os
import random
import ssl
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("Missing dependency. Run: pip install paho-mqtt")

SCENARIOS = ("normal", "spike", "flatline", "dropout", "burst", "drift")


def make_client(client_id):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:      # paho-mqtt 1.x
        return mqtt.Client(client_id=client_id)


def reading(fields, tick, elapsed, scenario, state):
    phase = elapsed / 600.0 * 2.0 * math.pi
    values = {}
    for index, name in enumerate(fields):
        if name == "temperature":
            value = 24.0 + 3.0 * math.sin(phase) + random.uniform(-0.2, 0.2)
        elif name == "humidity":
            value = 55.0 + 10.0 * math.sin(phase + 1.0) + random.uniform(-0.5, 0.5)
        else:
            value = 50.0 + 5.0 * math.sin(phase + index) + random.uniform(-0.5, 0.5)
        values[name] = value

    if scenario == "spike" and tick % 24 in (20, 21, 22):
        if "humidity" in values:
            values["humidity"] = 96.0
        if "temperature" in values:
            values["temperature"] += 8.0
    elif scenario == "drift" and "temperature" in values:
        values["temperature"] = min(90.0, values["temperature"] + 0.05 * tick)
    elif scenario == "flatline":
        if tick < 10:
            state["frozen"] = dict(values)
        else:
            values = dict(state["frozen"])

    return {name: round(value, 2) for name, value in values.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=os.environ.get("MQTT_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=8883)
    parser.add_argument("--cafile", default="/etc/mosquitto/certs/ca.crt")
    parser.add_argument("--ids", default="C45F1A90A994",
                        help="comma-separated device ids (default: your board's id)")
    parser.add_argument("--topic", default="devices/{id}/telemetry",
                        help="topic template; {id} is replaced per device")
    parser.add_argument("--fields", default="temperature,humidity")
    parser.add_argument("--scenario", choices=SCENARIOS, default="normal")
    parser.add_argument("--interval", type=float, default=5.0, help="seconds between messages")
    parser.add_argument("--duration", type=float, default=0.0, help="stop after N seconds (0 = forever)")
    parser.add_argument("--no-hostname-check", action="store_true",
                        help="verify the CA but not the host name (local testing only)")
    args = parser.parse_args()

    user = os.environ.get("MQTT_USER")
    password = os.environ.get("MQTT_PASS")
    if not user or not password:
        sys.exit("Set MQTT_USER and MQTT_PASS first (see the instructions).")

    fields = [name.strip() for name in args.fields.split(",") if name.strip()][:5]
    ids = [item.strip() for item in args.ids.split(",") if item.strip()]

    clients = {}
    for device_id in ids:
        client = make_client("sim-" + device_id)
        client.username_pw_set(user, password)
        client.tls_set(ca_certs=args.cafile, cert_reqs=ssl.CERT_REQUIRED)
        if args.no_hostname_check:
            client.tls_insecure_set(True)
        client.connect(args.host, args.port, keepalive=30)
        client.loop_start()
        clients[device_id] = {"client": client, "state": {}}

    print(f"Simulating {len(ids)} device(s), scenario={args.scenario}, every {args.interval}s. Ctrl+C to stop.")
    started = time.time()
    tick = 0
    try:
        while True:
            elapsed = time.time() - started
            if args.duration and elapsed >= args.duration:
                break
            silent = args.scenario == "dropout" and elapsed % 120.0 >= 60.0
            repeats = 20 if args.scenario == "burst" and tick % 12 == 0 else 1
            for device_id, item in clients.items():
                if silent:
                    print(f"[{device_id}] silent (dropout)")
                    continue
                topic = args.topic.format(id=device_id)
                for _ in range(repeats):
                    payload = json.dumps(reading(fields, tick, elapsed, args.scenario, item["state"]))
                    item["client"].publish(topic, payload)
                print(f"[{device_id}] {topic} x{repeats} {payload}")
            tick += 1
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        for item in clients.values():
            item["client"].loop_stop()
            item["client"].disconnect()
        print("Stopped.")


if __name__ == "__main__":
    main()
