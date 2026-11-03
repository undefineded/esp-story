"""One-off FRAM persistence check over the USB v4 protocol (CONFIG_ESP_CONSOLE_NONE).

Run with the build-usb-product image flashed:
    python tools/_fram_check.py COM5
"""
import json
import sys
import time
import zlib

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"


def frame(message):
    payload = json.dumps(message, separators=(",", ":")).encode()
    header = bytearray(b"SBX4")
    header += len(payload).to_bytes(2, "little")
    header += (0).to_bytes(2, "little")
    header += (zlib.crc32(bytes(header)) & 0xFFFFFFFF).to_bytes(4, "little")
    header += (zlib.crc32(payload) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(header) + payload


def read_messages(ser, seconds):
    deadline = time.time() + seconds
    buf = bytearray()
    messages = []
    raw = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            continue
        raw += chunk
        buf += chunk
        while True:
            start = buf.find(b"SBX4")
            if start < 0:
                if len(buf) > 3:
                    del buf[:-3]
                break
            if start:
                del buf[:start]
            if len(buf) < 16:
                break
            json_length = int.from_bytes(buf[4:6], "little")
            raw_length = int.from_bytes(buf[6:8], "little")
            if not (0 < json_length <= 24576 and raw_length <= 4096):
                del buf[:1]
                continue
            total = 16 + json_length + raw_length
            if len(buf) < total:
                break
            if (zlib.crc32(bytes(buf[:8])) & 0xFFFFFFFF) != int.from_bytes(buf[8:12], "little"):
                del buf[:1]
                continue
            payload = bytes(buf[16:16 + json_length])
            body = payload + bytes(buf[16 + json_length:total])
            if (zlib.crc32(body) & 0xFFFFFFFF) != int.from_bytes(buf[12:16], "little"):
                del buf[:1]
                continue
            del buf[:total]
            try:
                messages.append(json.loads(payload))
            except Exception:
                pass
    return messages, bytes(raw)


def request(ser, message, timeout=2.5):
    ser.write(frame(message))
    ser.flush()
    messages, raw = read_messages(ser, timeout)
    for item in messages:
        if isinstance(item, dict) and item.get("id") == message["id"]:
            return item, raw
    return (messages[-1] if messages else None), raw


def stats(ser, identifier, timeout=2.5):
    response, raw = request(ser, {"v": 4, "id": identifier, "cmd": "sys", "act": "stats"}, timeout)
    return response, raw


def open_port():
    port = serial.Serial(PORT, 115200, timeout=0.2)
    time.sleep(0.5)
    return port


def wait_stats(ser, identifier, seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        response, raw = stats(ser, identifier, 1.5)
        if isinstance(response, dict) and "settings" in response:
            return response, raw
    return None, b""


ser = open_port()
response, raw = wait_stats(ser, 1)
if not response:
    print("FAIL: no sys stats response")
    sys.exit(1)

if "--restore" in sys.argv:
    # Put the board back to the settings it had before the persistence test
    # (the user asked not to change the volume at night).
    print("before restore:", json.dumps(response["settings"], ensure_ascii=False))
    request(ser, {"v": 4, "id": 10, "cmd": "sys", "act": "set",
                  "brightness": 40, "eq": 0, "rotation": 3})
    request(ser, {"v": 4, "id": 11, "cmd": "sys", "act": "set", "volume": 0})
    time.sleep(2.0)
    response, _ = stats(ser, 12)
    print("restored:", json.dumps(response["settings"], ensure_ascii=False))
    ser.close()
    sys.exit(0)

print("boot settings:", json.dumps(response["settings"], ensure_ascii=False))
print("fram log:", "FRAM online" in raw.decode("utf-8", "ignore"),
      "/", "FRAM ready" in raw.decode("utf-8", "ignore"))

wanted = {"brightness": 55, "eq": "bass", "rotation": 1}
response, _ = request(ser, {"v": 4, "id": 2, "cmd": "sys", "act": "set",
                            "brightness": 55, "eq": 2, "rotation": 1})
print("set response ok:", response.get("ok") if response else None)
response, _ = request(ser, {"v": 4, "id": 3, "cmd": "sys", "act": "set", "volume": 7})
print("volume response ok:", response.get("ok") if response else None)
time.sleep(2.0)

response, _ = stats(ser, 4)
before = response["settings"] if response and "settings" in response else {}
print("after set:", json.dumps(before, ensure_ascii=False))

response, _ = request(ser, {"v": 4, "id": 5, "cmd": "sys", "act": "reboot"})
print("reboot response ok:", response.get("ok") if response else None)
ser.close()
time.sleep(4.0)

ser = open_port()
response, raw = wait_stats(ser, 6)
after = response["settings"] if response and "settings" in response else {}
print("after reboot:", json.dumps(after, ensure_ascii=False))
ser.close()

ok = all(after.get(key) == value for key, value in wanted.items()) and after.get("volume") == 7
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 2)
