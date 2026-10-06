#!/usr/bin/env python3
"""通过实际 QEMU、原始应用和虚拟 evdev 验证运行与按键反馈。"""
import argparse
import base64
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8765")
    args = parser.parse_args()
    url = args.url.rstrip("/")

    def get(path):
        with urllib.request.urlopen(url + path, timeout=5) as response: return json.load(response)

    token = get("/api/state")["token"]

    def post(path, data):
        request = urllib.request.Request(url + path, json.dumps(data).encode(),
                                         {"Content-Type": "application/json", "X-C1Sim-Token": token})
        with urllib.request.urlopen(request, timeout=5) as response: return json.load(response)

    def wait(test, description, timeout=60, expected_app=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state, frame = get("/api/state"), get("/api/frame")
            if test(state, frame): return state, frame
            if state["exit_code"] is not None and (expected_app is None or state["app"] == expected_app):
                raise AssertionError(description + "：程序退出\n" + "\n".join(state["logs"][-6:]))
            time.sleep(0.1)
        raise AssertionError("等待超时：" + description)

    def digest(frame): return hashlib.sha256(base64.b64decode(frame["pixels"])).hexdigest()

    def key(code):
        post("/api/key", {"code": code, "value": 1})
        time.sleep(0.08)
        post("/api/key", {"code": code, "value": 0})

    results = []
    wait(lambda state, frame: state["ready"], "MIPS 客体连接")
    apps = ["launcher", "hello", "book-reader", "pic", "chichugames", "music-player", "pinao"]
    if any(app["id"] == "custom" for app in get("/api/state")["apps"]): apps.append("custom")
    for app in apps:
        before = get("/api/frame")["sequence"]
        post("/api/run", {"id": app})
        state, frame = wait(lambda state, frame: state["app"] == app and state["pid"] > 0 and frame["sequence"] > before,
                            app + " 原始二进制启动", 15, expected_app=app)
        time.sleep(0.4)
        state, frame = get("/api/state"), get("/api/frame")
        if state["exit_code"] is not None: raise AssertionError(app + " 启动后退出：" + str(state["logs"][-5:]))
        pixels = base64.b64decode(frame["pixels"])
        if len(pixels) != 5624 or len(set(pixels)) < 2: raise AssertionError(app + " 没有输出有效界面")
        results.append({"app": app, "pid": state["pid"], "sequence": frame["sequence"], "frame_sha256": digest(frame)})
        if app in ["hello", "book-reader", "pic", "music-player", "pinao"]:
            before_key = frame["sequence"]
            code = {"hello":106, "book-reader":28, "pic":28, "music-player":115, "pinao":30}[app]
            post("/api/key", {"code": code, "value": 1})
            try:
                wait(lambda state, frame: frame["sequence"] > before_key, app + " 按键反馈", 5, expected_app=app)
            finally: post("/api/key", {"code": code, "value": 0})
            results.append({"check": app + "-key-feedback", "code": code, "passed": True})
        subprocess.run([sys.executable, str(ROOT / "scripts/capture_frame.py"),
                        str(ROOT / "build/verification" / (app + ".png")), "--url", url], check=True, stdout=subprocess.DEVNULL)
        print("已运行原始 MIPS 程序：" + app, flush=True)
    post("/api/run", {"id": "launcher"})
    _, frame = wait(lambda state, frame: state["app"] == "launcher" and state["pid"] > 0, "恢复启动器")
    time.sleep(0.5)
    frame = get("/api/frame")
    previous = digest(frame)
    key(116)
    _, locked = wait(lambda state, frame: digest(frame) != previous, "电源键进入锁屏", 5)
    key(116)
    wait(lambda state, frame: digest(frame) != digest(locked), "电源键解锁", 5)
    results.append({"check": "power-key-lock-unlock", "passed": True})
    audit = get("/api/binaries")
    command = "sha256sum " + " ".join(shlex.quote(row["guest_path"]) for row in audit)
    output = subprocess.run([sys.executable, str(ROOT / "scripts/console.py"), "--url", url, command],
                            check=True, capture_output=True, text=True).stdout
    actual = {path: digest for digest, path in re.findall(r"^([0-9a-f]{64})  (.+)$", output, re.MULTILINE)}
    for row in audit:
        if actual.get(row["guest_path"]) != row["sha256"]: raise AssertionError("客体二进制与原始文件不一致：" + row["id"])
    results.append({"check": "guest-original-binary-sha256", "count": len(audit), "passed": True})
    destination = ROOT / "build/verification/smoke.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps({"engine": "qemu-system-mipsel", "cpu": "24Kf", "results": results}, ensure_ascii=False, indent=2) + "\n")
    print("验证记录：" + str(destination))


if __name__ == "__main__": main()
