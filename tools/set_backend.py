import sys, json, time, glob, getpass
import serial

url = input("Backend URL [http://172.16.124.161:8080]: ").strip() or "http://172.16.124.161:8080"
key = getpass.getpass("API key (hidden, paste then Enter): ").strip()
tz = "EAT-3"

print(f"Key length: {len(key)} (must be 32-64)")
if not 32 <= len(key) <= 64:
    sys.exit("Key length is wrong, nothing was sent.")
if not url.startswith(("http://", "https://")):
    sys.exit("URL must start with http:// or https://")

ports = sorted(glob.glob("/dev/ttyACM*"))
if not ports:
    sys.exit("No /dev/ttyACM* port found. Is the board plugged in?")

s = serial.Serial()
s.port, s.baudrate, s.timeout = ports[0], 115200, 0.5
s.dtr = False
s.rts = False
s.open()
print("--- using", ports[0])

# The board resets when the port opens: wait until it has finished booting.
end = time.time() + 12
while time.time() < end:
    line = s.readline().decode(errors="replace").rstrip()
    if "USB provisioning ready" in line:
        break
time.sleep(0.5)

def send(text, wait=3):
    s.write((text + "\n").encode())
    end = time.time() + wait
    while time.time() < end:
        out = s.readline().decode(errors="replace").rstrip()
        if out and "select returned" not in out:
            print(out)

send("CONFIGURE_BACKEND " + json.dumps({"backendUrl": url, "apiKey": key, "timezone": tz}, separators=(",", ":")))
send("STATUS")
s.close()
