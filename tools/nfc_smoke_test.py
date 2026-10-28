#!/usr/bin/env python3
"""PN532 + StoryBox NFC 串口冒烟测试 (需要 pyserial)

用法:
    python tools/nfc_smoke_test.py --port COM7
    python tools/nfc_smoke_test.py --port COM7 --text "月亮邮递员 · 第 03 集"
    python tools/nfc_smoke_test.py --port COM7 --uri "https://storybox.local/album/nursery"
    python tools/nfc_smoke_test.py --port COM7 --read-pages 4,36

脚本只依赖固件的 USB CDC JSON 协议；不连接 PN532 时会明确提示“模块未检测到”。
"""
import argparse
import json
import sys
import time
from typing import Optional

try:
    import serial
except ImportError:  # pragma: no cover
    print("缺少 pyserial: python -m pip install pyserial", file=sys.stderr)
    raise SystemExit(2)

PREFIX_HTTP = "http://"
PREFIX_HTTPS = "https://"


class NfcDevice:
    def __init__(self, ser: "serial.Serial", verbose: bool = False):
        self.ser = ser
        self.verbose = verbose
        self.seq = 0
        self.events: list = []

    def _read_line(self, deadline: float) -> Optional[dict]:
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if not line.startswith("{"):
                if self.verbose:
                    print(f"[dev] {line}")
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                if self.verbose:
                    print(f"[json?] {line}")
                continue
            if msg.get("evt") == "nfc":
                self.events.append(msg)
            return msg
        return None

    def request(self, cmd: str, act: str, timeout: float = 5.0, **kwargs):
        self.seq += 1
        req_id = self.seq
        payload = {"v": 2, "id": req_id, "cmd": cmd, "act": act, **kwargs}
        self.ser.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
        self.ser.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self._read_line(deadline)
            if msg is None:
                break
            if msg.get("id") == req_id:
                if msg.get("ok") is False:
                    raise RuntimeError(f"{cmd}.{act} 失败: {msg.get('err')} {msg.get('msg', '')}")
                return msg
        raise TimeoutError(f"{cmd}.{act} 超时 ({timeout:.1f}s)")

    def pump(self, seconds: float):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self._read_line(min(deadline, time.monotonic() + 0.2))

    def wait_event(self, evt_type: str, timeout: float):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while self.events:
                event = self.events.pop(0)
                print(f"[event] {event.get('type')} {json.dumps({k: v for k, v in event.items() if k not in ('v', 'evt')}, ensure_ascii=False)}")
                if event.get("type") == evt_type:
                    return event
            self._read_line(min(deadline, time.monotonic() + 0.2))
        return None


def ndef_text_record(text: str, language: str = "zh") -> bytes:
    lang = language.encode("utf-8")
    body = text.encode("utf-8")
    payload = bytes([len(lang) & 0x3F]) + lang + body
    type_bytes = b"T"
    if len(payload) < 256:
        return bytes([0xD1, len(type_bytes), len(payload)]) + type_bytes + payload
    length = len(payload)
    return (
        bytes([0xC1, len(type_bytes)])
        + length.to_bytes(4, "big")
        + type_bytes
        + payload
    )


def ndef_uri_record(uri: str) -> bytes:
    if uri.startswith(PREFIX_HTTPS):
        prefix, rest = 0x04, uri[len(PREFIX_HTTPS):]
    elif uri.startswith(PREFIX_HTTP):
        prefix, rest = 0x03, uri[len(PREFIX_HTTP):]
    else:
        prefix, rest = 0x00, uri
    payload = bytes([prefix]) + rest.encode("utf-8")
    type_bytes = b"U"
    if len(payload) < 256:
        return bytes([0xD1, len(type_bytes), len(payload)]) + type_bytes + payload
    return bytes([0xC1, len(type_bytes)]) + len(payload).to_bytes(4, "big") + type_bytes + payload


def print_card(card: dict) -> None:
    print(
        f"[card] {card.get('type')}  UID={card.get('uid')}  "
        f"ATQA={card.get('atqa')}  SAK={card.get('sak')}  "
        f"cap={card.get('cap')}B  writable={card.get('writable')}  formatted={card.get('formatted')}"
    )
    if card.get("version"):
        print(f"[card] version={card['version']}  cc={card.get('cc')}  lock={card.get('lock')}")


def main() -> int:
    parser = argparse.ArgumentParser(description="StoryBox PN532 NFC 串口冒烟测试")
    parser.add_argument("--port", required=True, help="串口，例如 COM7 或 /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--text", help="读取到卡片后写入这段文本")
    parser.add_argument("--uri", help="读取到卡片后写入这个链接")
    parser.add_argument("--read-pages", help="读取 Type 2 页范围，例如 4,36")
    parser.add_argument("--timeout", type=float, default=15.0, help="等待卡片/事件的秒数")
    parser.add_argument("--verbose", action="store_true", help="打印设备普通日志")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:  # noqa: BLE001
        print(f"打开串口失败: {exc}", file=sys.stderr)
        return 1

    device = NfcDevice(ser, verbose=args.verbose)
    print(f"已打开 {args.port} @ {args.baud}")
    try:
        # 1. 握手；刚打开 CDC 时设备可能正在启动，最多重试几次
        hello = None
        for attempt in range(5):
            try:
                hello = device.request("sys", "hello", timeout=2.0)
                break
            except TimeoutError:
                print(f"等待 hello... ({attempt + 1}/5)")
        if hello is None:
            print("设备没有响应 hello；确认固件包含 hostlink.cpp，串口未被占用。", file=sys.stderr)
            return 1
        print(f"[hello] {hello.get('name')}  fw={hello.get('fw')}  board={hello.get('board')}")
        print(f"[hello] features={hello.get('features')}")
        existing_card = hello.get("nfc", {}).get("card")
        if existing_card:
            print_card(existing_card)

        # 2. 模块状态
        status = device.request("nfc", "status")
        nfc = status.get("data", {})
        print(f"[status] present={nfc.get('present')}  chip={nfc.get('chip')}  fw={nfc.get('fw')}  polling={nfc.get('polling')}")
        if not nfc.get("present"):
            print("未检测到 PN532。检查 3.3V、SDA/SCL、I2C 拨码，然后重试。", file=sys.stderr)
            return 2

        # 3. 开始轮询并等卡
        device.request("nfc", "poll", on=True)
        print(f"请把卡片放到 PN532 上，等待 {args.timeout:.0f}s ...")
        card_event = device.wait_event("card", args.timeout)
        if card_event is None:
            print("没有检测到卡片。", file=sys.stderr)
            return 3
        card = card_event["card"]
        print_card(card)

        # 4. 可选写入
        message = None
        if args.text:
            message = ndef_text_record(args.text)
            print(f"[write] 文本 {args.text!r}, NDEF {len(message)}B")
        elif args.uri:
            message = ndef_uri_record(args.uri)
            print(f"[write] 链接 {args.uri!r}, NDEF {len(message)}B")
        if message is not None:
            if not card.get("type2"):
                print("当前卡片不是 Type 2，脚本不执行 NDEF 写入。", file=sys.stderr)
                return 4
            response = device.request(
                "nfc", "write_ndef", ndef=message.hex().upper(), verify=True, timeout=6.0
            )
            print(f"[write] accepted, len={response.get('len')}")
            result = device.wait_event("write", args.timeout)
            if result is None:
                print("等待写入结果超时。", file=sys.stderr)
                return 5
            if result.get("phase") == "error":
                print(f"写入失败: {result.get('code')} {result.get('msg', '')}", file=sys.stderr)
                return 6
            device.request("nfc", "refresh")
            refreshed = device.wait_event("card", 5.0)
            if refreshed:
                print_card(refreshed["card"])
                print(f"[write] new user data: {refreshed['card'].get('data', '')[:96]}...")

        # 5. 可选原始页读取
        if args.read_pages:
            start_s, count_s = args.read_pages.split(",", 1)
            start, count = int(start_s), int(count_s)
            data = device.request("nfc", "read", kind="page", start=start, count=count).get("data", {})
            payload = bytes.fromhex(data.get("hex", ""))
            print(f"[raw] page {start} +{count} ({len(payload)}B)")
            for offset in range(0, len(payload), 16):
                chunk = payload[offset:offset + 16]
                hexpart = " ".join(f"{b:02X}" for b in chunk)
                ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                print(f"  0x{offset:04X}  {hexpart:<47}  {ascii_part}")

        print("完成。")
        return 0
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # noqa: BLE001
        print(f"失败: {exc}", file=sys.stderr)
        return 1
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
