#!/usr/bin/env python3
"""Push the MQTT password (and optionally the user name) to the board over USB.

Reads MQTT_PASS (and MQTT_USER, if set) from the environment so nothing secret
appears on the command line. Fields not sent keep their saved values.
Close any serial monitor first: it holds the port.
"""
import argparse
import glob
import json
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("Missing dependency. Run: pip install pyserial")


def find_port(requested):
    if requested:
        return requested
    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
    if not ports:
        sys.exit("No serial port found. Is the board plugged in?")
    return ports[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port (default: first /dev/ttyACM* or ttyUSB*)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=10.0, help="seconds to wait for the reply")
    args = parser.parse_args()

    password = os.environ.get("MQTT_PASS")
    if not password:
        sys.exit("Set MQTT_PASS first (read -rsp 'MQTT password: ' MQTT_PASS; export MQTT_PASS).")
    if "\n" in password or "\r" in password:
        sys.exit("The password must not contain line breaks.")

    payload = {"password": password}
    user = os.environ.get("MQTT_USER")
    if user:
        payload["username"] = user
    command = "CONFIGURE_MQTT " + json.dumps(payload) + "\n"

    port = find_port(args.port)
    link = serial.Serial()
    link.port = port
    link.baudrate = args.baud
    link.timeout = 0.5
    link.dtr = False
    link.rts = False
    try:
        link.open()
    except serial.SerialException as error:
        sys.exit(f"Cannot open {port}: {error}\nClose any serial monitor and try again.")

    time.sleep(0.5)
    link.reset_input_buffer()
    link.write(command.encode("utf-8"))
    link.flush()

    result = None
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        line = link.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("USB MQTT settings saved"):
            result = line
            break
        if line.startswith("ERROR: Could not save MQTT") or line.startswith("ERROR: CONFIGURE_MQTT") \
                or " must be " in line:
            print(line)
            link.close()
            sys.exit(1)
    link.close()

    if result is None:
        sys.exit("No confirmation from the board. Check the port and that the firmware is running.")
    print(result)


if __name__ == "__main__":
    main()
