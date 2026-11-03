#!/usr/bin/env python3
"""通过 USB CDC 串口把文件写进故事盒的 TF 卡 (SBX4 协议 v4)。

固件侧实现见 src/filexfer.cpp / src/hostlink.cpp:
  frame = "SBX4" | jsonLen:u16 | rawLen:u16 | crc32(前8字节):u32 | crc32(JSON+raw):u32 | JSON | raw
  fs.session_begin -> fs.write_begin{path,size,overwrite} -> fs.write_chunk{xid,offset}+raw(<=4096)
  -> fs.write_end{xid} -> fs.session_end

用法:
  python tools/upload_serial.py COM5 "out-video/xxx.avi" "out-video/xxx.mp3"
  python tools/upload_serial.py COM5 --dir /video --window 4 out-video/*.avi

pyserial 在 PlatformIO 的 penv 里:
  & "$env:USERPROFILE\\.platformio\\penv\\Scripts\\python.exe" tools/upload_serial.py ...
"""
import argparse
import json
import os
import struct
import sys
import time
import zlib

import serial

HEADER = 16
MAX_JSON = 24576
MAX_RAW = 4096
CHUNK = 4096


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def build_frame(doc, raw=b""):
    js = json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    head = b"SBX4" + struct.pack("<HH", len(js), len(raw))
    head += struct.pack("<I", crc32(head)) + struct.pack("<I", crc32(js + raw))
    return head + js + raw


class Link:
    def __init__(self, port, baud=115200):
        self.buf = bytearray()
        self.next_id = 1
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.02
        self.ser.write_timeout = 10
        # USB-Serial-JTAG 上 DTR/RTS 会驱动 EN/IO0，打开前先置低，避免进下载模式
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def new_id(self):
        self.next_id += 1
        return self.next_id

    def send(self, doc, raw=b""):
        self.ser.write(build_frame(doc, raw))

    def _pump(self):
        data = self.ser.read(65536)
        if data:
            self.buf += data
        return bool(data)

    def read_frame(self):
        """返回 (json, raw) 或 None"""
        while True:
            i = self.buf.find(b"SBX4")
            if i < 0:
                if len(self.buf) > 3:
                    self.buf = self.buf[-3:]
                return None
            if i:
                del self.buf[:i]
            if len(self.buf) < HEADER:
                return None
            jl, rl = struct.unpack_from("<HH", self.buf, 4)
            if jl == 0 or jl > MAX_JSON or rl > MAX_RAW:
                del self.buf[:4]
                continue
            total = HEADER + jl + rl
            if len(self.buf) < total:
                return None
            if struct.unpack_from("<I", self.buf, 8)[0] != crc32(bytes(self.buf[:8])):
                del self.buf[:4]
                continue
            body = bytes(self.buf[HEADER:total])
            if struct.unpack_from("<I", self.buf, 12)[0] != crc32(body):
                del self.buf[:4]
                continue
            del self.buf[:total]
            try:
                doc = json.loads(body[:jl].decode("utf-8"))
            except Exception:
                continue
            return doc, body[jl:]

    def wait_reply(self, want_id, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            doc = self.read_frame()
            if doc is not None:
                js, _raw = doc
                if js.get("id") == want_id:
                    return js
                # 设备主动事件（boot/nfc）没有 id，忽略
                continue
            if not self._pump():
                time.sleep(0.002)
        raise TimeoutError(f"等待响应超时 (id={want_id})")

    def request(self, doc, timeout=10.0, raw=b""):
        doc = dict(doc)
        doc.setdefault("v", 4)
        doc["id"] = self.new_id()
        self.send(doc, raw)
        js = self.wait_reply(doc["id"], timeout)
        if not js.get("ok"):
            raise RuntimeError(f"{doc.get('act')} 失败: {js.get('err')} {js.get('msg', '')}")
        return js


def upload(link, local, remote, window=4, retries=2):
    size = os.path.getsize(local)
    name = os.path.basename(local)
    print(f"\n{local} -> {remote}  ({size/1024/1024:.1f} MB)")
    begin = link.request({"cmd": "fs", "act": "write_begin",
                          "path": remote, "size": size, "overwrite": True}, timeout=15)
    xid = begin["data"]["xid"]
    t0 = time.time()
    sent = 0
    confirmed = 0
    pending = []          # [(id, offset, length)]
    f = open(local, "rb")
    try:
        while confirmed < size:
            while len(pending) < window:
                data = f.read(CHUNK)
                if not data:
                    break
                req = {"v": 4, "id": link.new_id(), "cmd": "fs", "act": "write_chunk",
                       "xid": xid, "offset": sent}
                link.send(req, data)
                pending.append((req["id"], sent, len(data)))
                sent += len(data)
            if not pending:
                break
            pid, off, ln = pending[0]
            try:
                js = link.wait_reply(pid, 20)
            except TimeoutError:
                if retries <= 0:
                    raise
                retries -= 1
                # 重发同一偏移：固件对已写入的块是幂等的
                with open(local, "rb") as rf:
                    rf.seek(off)
                    data = rf.read(ln)
                req = {"v": 4, "id": link.new_id(), "cmd": "fs", "act": "write_chunk",
                       "xid": xid, "offset": off}
                link.send(req, data)
                pending[0] = (req["id"], off, ln)
                continue
            if not js.get("ok"):
                raise RuntimeError(f"write_chunk 失败: {js.get('err')}")
            confirmed = js.get("data", {}).get("written", off + ln)
            pending.pop(0)
            done = confirmed
            if done % (CHUNK * 64) < CHUNK or done >= size:
                el = max(0.001, time.time() - t0)
                print(f"\r  {done*100//size:3d}%  {done/1024/1024:6.2f}/{size/1024/1024:.2f} MB"
                      f"  {done/1024/el:6.1f} KB/s", end="", flush=True)
        print()
        end = link.request({"cmd": "fs", "act": "write_end", "xid": xid}, timeout=30)
        if end.get("data", {}).get("written", confirmed) != size:
            print(f"  警告: 设备确认 {end.get('data')}，本地 {size}")
        el = time.time() - t0
        print(f"  完成 {size/1024/1024:.2f} MB，{el:.1f}s，{size/1024/el:.1f} KB/s")
    finally:
        f.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--dir", default="/video")
    ap.add_argument("--window", type=int, default=4)
    ap.add_argument("--no-session", action="store_true")
    args = ap.parse_args()

    link = Link(args.port)
    try:
        hello = link.request({"cmd": "hello", "act": "hello"}, timeout=5)
        print(f"设备在线: {hello.get('fw', '?')} proto={hello.get('proto')}")

        if not args.no_session:
            link.request({"cmd": "fs", "act": "session_begin"}, timeout=15)
            print("会话已开始（播放已停止）")
        try:
            for local in args.files:
                remote = args.dir.rstrip("/") + "/" + os.path.basename(local)
                upload(link, local, remote, window=max(1, args.window))
        finally:
            if not args.no_session:
                link.request({"cmd": "fs", "act": "session_end"}, timeout=30)
                print("会话结束")
    finally:
        link.close()


if __name__ == "__main__":
    main()
