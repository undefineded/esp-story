#!/usr/bin/env python3
"""故事机 BLE 冒烟测试 (Windows/Linux/macOS, 需要 bleak)

用法:
    python tools/ble_smoke_test.py --name StoryBox-CBD5
    python tools/ble_smoke_test.py --name StoryBox-CBD5 --reset

不传 --reset 时会尽量恢复设备原有家长配置；传 --reset 会在测试结束后
清空绑定、PIN、额度与白名单，方便再走一遍首次绑定流程。
"""
import argparse
import asyncio
import json
import time
from typing import Optional

from bleak import BleakClient, BleakScanner

SERVICE = "6b7a0001-3c4d-4e5f-8a9b-0c1d2e3f4a5b"
RX = "6b7a0002-3c4d-4e5f-8a9b-0c1d2e3f4a5b"
TX = "6b7a0003-3c4d-4e5f-8a9b-0c1d2e3f4a5b"


class StoryBox:
    def __init__(self, client: BleakClient):
        self.client = client
        self.seq = 0
        self.rx = ""
        self.lines: list = []
        self.status: Optional[dict] = None

    async def start(self):
        # Windows 上刚连上时 GATT/CCCD 可能还没就绪，稍等再订阅
        await asyncio.sleep(1.5)
        await self.client.start_notify(TX, self._on_notify)
        print("notify subscribed")
        await asyncio.sleep(0.8)

    def _on_notify(self, _char, data: bytearray):
        self.rx += data.decode("utf-8", errors="ignore")
        while "\n" in self.rx:
            line, self.rx = self.rx.split("\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except Exception as exc:
                print("json err", repr(exc), repr(line))
                continue
            if msg.get("evt") == "state":
                self.status = msg
            self.lines.append(msg)

    async def request(self, cmd: str, **kwargs):
        self.seq += 1
        req_id = self.seq
        payload = {"v": 1, "id": req_id, "cmd": cmd, **kwargs}
        await self.client.write_gatt_char(
            RX, (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"), response=True
        )
        deadline = time.monotonic() + 6
        while True:
            for i, msg in enumerate(self.lines):
                if msg.get("id") == req_id:
                    self.lines.pop(i)
                    if msg.get("ok") is False:
                        raise RuntimeError(f"{cmd} failed: {msg.get('err')}")
                    return msg
            if time.monotonic() >= deadline:
                raise TimeoutError(f"{cmd} timeout")
            await asyncio.sleep(0.05)

    async def hello(self):
        return await self.request("hello")

    async def auth(self, token: str):
        return await self.request("auth", token=token)

    async def bind(self, code: str):
        return await self.request("bind", code=code)

    async def sync_time(self):
        epoch = int(time.time())
        tz = -time.altzone // 60 if time.daylight and time.localtime().tm_isdst else -time.timezone // 60
        return await self.request("sync", epoch=epoch, tz=tz)


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (("  " + detail) if detail else ""))
    return ok


async def find_device(name_prefix: str):
    print(f"scanning for {name_prefix}...")
    devices = await BleakScanner.discover(timeout=10)
    for d in devices:
        if d.name and d.name.startswith(name_prefix):
            return d
    raise RuntimeError("StoryBox not found")


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="StoryBox-", help="name or prefix")
    parser.add_argument("--reset", action="store_true", help="factory reset after tests")
    args = parser.parse_args()

    dev = await find_device(args.name)
    print("found", dev.name, dev.address)
    client = BleakClient(dev, timeout=20)
    await client.connect()
    print("connected, mtu:", getattr(client, "mtu_size", "?"))
    box = StoryBox(client)
    await box.start()
    results = []

    hello = await box.hello()
    print("hello:", hello)
    token = None
    if not hello.get("bound"):
        code = hello.get("code")
        if not code:
            raise RuntimeError("unbound but no bind code")
        bound = await box.bind(code)
        token = bound.get("token")
        print("bound, token:", token)
        results.append(check("bind", bool(token)))
    else:
        raise RuntimeError("device already bound; use --reset on a previously bound device or erase FRAM")

    await box.auth(token)
    await box.sync_time()
    results.append(check("auth", True))

    cfg = (await box.request("get", what="config"))["data"]
    print("config:", json.dumps(cfg, ensure_ascii=False))
    state = (await box.request("get", what="state"))["data"]
    print("state:", json.dumps(state, ensure_ascii=False))
    results.append(check("state", state.get("vol") is not None))

    stories = (await box.request("get", what="stories", offset=0, limit=4))["data"]
    cats = (await box.request("get", what="cats", offset=0, limit=4))["data"]
    print("stories:", json.dumps(stories, ensure_ascii=False))
    print("cats:", json.dumps(cats, ensure_ascii=False))
    results.append(check("catalog", stories.get("total", 0) >= 0 and cats.get("total", 0) >= 0))

    # volume range 5..8, then clamp 15 -> 8 and 0 -> 5
    await box.request("set", volMin=5, volMax=8)
    await box.request("ctrl", action="volume", value=15)
    st = (await box.request("get", what="state"))["data"]
    results.append(check("volume max clamp", st.get("vol") == 8, f"vol={st.get('vol')}"))
    await box.request("ctrl", action="volume", value=0)
    st = (await box.request("get", what="state"))["data"]
    results.append(check("volume min clamp", st.get("vol") == 5, f"vol={st.get('vol')}"))

    # remote lock
    await box.request("set", remoteLocked=True)
    st = (await box.request("get", what="state"))["data"]
    results.append(check("remote lock", st.get("lock") == 3, f"lock={st.get('lock')}"))
    await box.request("set", remoteLocked=False)
    st = (await box.request("get", what="state"))["data"]
    results.append(check("remote unlock", st.get("lock") == 0, f"lock={st.get('lock')}"))

    # allowlist
    await box.request("set", allowlistEnabled=True, allowStories=[0], allowCats=[1])
    sts = (await box.request("get", what="stories", offset=0, limit=4))["data"]
    cts = (await box.request("get", what="cats", offset=0, limit=4))["data"]
    allowed_story = [x for x in sts.get("items", []) if x.get("a")]
    allowed_cat = [x for x in cts.get("items", []) if x.get("a")]
    results.append(check("story allowlist", len(allowed_story) <= 1, f"allowed={len(allowed_story)}"))
    results.append(check("cat allowlist", len(allowed_cat) <= 1, f"allowed={len(allowed_cat)}"))
    await box.request("set", allowlistEnabled=False, allowStories=[], allowCats=[])

    # daily limit
    await box.request("set", dailyLimitMin=15)
    st = (await box.request("get", what="state"))["data"]
    results.append(check("daily limit", st.get("limit") == 900, f"limit={st.get('limit')}"))

    # PIN
    await box.request("pin", action="set", pin="2468")
    ok = (await box.request("pin", action="verify", pin="2468")).get("valid")
    bad = (await box.request("pin", action="verify", pin="0000")).get("valid")
    results.append(check("pin set/verify", bool(ok) and not bad, f"ok={ok} bad={bad}"))

    # story playback (if stories exist)
    if stories.get("total", 0) > 0:
        first = stories["items"][0]["i"]
        await box.request("ctrl", action="story", value=first)
        await asyncio.sleep(1.2)
        st = (await box.request("get", what="state"))["data"]
        results.append(check("story play", st.get("mode") == "story", f"mode={st.get('mode')}"))
        await box.request("ctrl", action="pause")
    else:
        print("SKIP story play (no stories)")

    # report
    report = (await box.request("get", what="report"))["data"]
    print("report:", json.dumps(report, ensure_ascii=False))
    results.append(check("report", "today" in report))

    # restore defaults (or factory reset)
    if args.reset:
        await box.request("reset")
        print("factory reset sent")
        results.append(check("factory reset", True))
    else:
        await box.request(
            "set",
            dailyLimitMin=cfg.get("dailyLimitMin", 30),
            volMin=cfg.get("volMin", 3),
            volMax=cfg.get("volMax", 12),
            bedtimeEnabled=cfg.get("bedtimeEnabled", False),
            bedStartMin=cfg.get("bedStartMin", 1230),
            bedEndMin=cfg.get("bedEndMin", 420),
            allowlistEnabled=False,
            remoteLocked=False,
        )
        print("original config restored")

    await client.disconnect()
    print("\n%d/%d checks passed" % (sum(1 for r in results if r), len(results)))
    raise SystemExit(0 if all(results) else 1)


if __name__ == "__main__":
    asyncio.run(main())
