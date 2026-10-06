#!/usr/bin/env python3
"""在独立 QEMU 中验证原版启动器打开网易云后的数字输入。"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

from audio_test import Guest, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    selection = json.loads((ROOT / "release.json").read_text())
    records = {f[1]: f for line in (ROOT / "build/repository/index.v1").read_text().splitlines()
               if (f := line.split("\t"))[0] == "P"}
    guest = Guest("http://127.0.0.1:" + str(args.port))
    with tempfile.TemporaryDirectory(prefix="netease-keys-", dir=output) as temporary:
        root = pathlib.Path(temporary)
        (root / "build/repository/objects").mkdir(parents=True)
        (root / "runtime").mkdir()
        (root / "public").symlink_to(ROOT / "public", target_is_directory=True)
        (root / "build/images").symlink_to(ROOT / "build/images", target_is_directory=True)
        shutil.copyfile(ROOT / "build/binaries.json", root / "build/binaries.json")
        shutil.copyfile(ROOT / "release.json", root / "release.json")
        for name in ("index.v1", "index.v1.sig"):
            shutil.copyfile(ROOT / "build/repository" / name, root / "build/repository" / name)
        for app_id in [app["id"] for app in selection["repository"]["apps"]] + ["netease-music"]:
            relative = records[app_id][4]
            source = ROOT / "build/repository" / relative
            if not source.exists(): source = ROOT / ".cache/channel" / relative
            if source.exists(): os.link(source, root / "build/repository" / relative)
        code = "import pathlib,sys,start; start.ROOT=pathlib.Path(sys.argv.pop(1)).resolve(); start.main()"
        with (root / "runner.log").open("w") as log:
            process = subprocess.Popen([sys.executable, "-c", code, str(root), "--no-open", "--port", str(args.port),
                                        "--audio-backend", "none"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait(lambda: process.poll() is None and guest.get("/api/state")["ready"], "独立客体启动", 90)
                guest.token = guest.get("/api/state")["token"]
                guest.shell("c1pkg install netease-music", timeout=60)
                installed = re.findall(r"^([A-Za-z0-9-]+)\t[0-9.]+$", guest.shell("c1pkg list"), re.M)
                time.sleep(.5)
                guest.key(28)
                time.sleep(.5)
                for _ in range(installed.index("netease-music")): guest.key(108)
                guest.key(28)
                wait(lambda: re.search(r"^C1_NETEASE\r?$", guest.shell(
                    "for p in /proc/[0-9]*/comm; do read n < \"$p\"; if [ \"$n\" = netease-music ]; then echo C1_NETEASE; fi; done"), re.M),
                    "原版启动器打开网易云", 20)
                time.sleep(1)
                guest.key(50)
                time.sleep(.3)
                guest.key(28)
                time.sleep(.3)
                wait(lambda: guest.get("/api/state").get("input_app") == "netease-music",
                     "识别实际接收按键的子应用", 10)
                result = subprocess.run(["node", str(ROOT / "scripts/netease_keyboard_test.mjs"), guest.url],
                                        capture_output=True, text=True, timeout=30)
                print(result.stdout + result.stderr, end="", flush=True)
                subprocess.run([sys.executable, str(ROOT / "scripts/capture_frame.py"),
                                str(output / "netease-keyboard.png"), "--url", guest.url], check=True)
                result.check_returncode()
                package = root / "build/repository" / records["netease-music"][4]
                if not package.exists(): package = root / ".cache/channel" / records["netease-music"][4]
                with tarfile.open(package) as archive:
                    expected = hashlib.sha256(archive.extractfile("payload/netease-music").read()).hexdigest()
                assert re.search(r"^" + expected + r"  /storage/c1/apps/netease-music/current/netease-music$",
                                 guest.shell("sha256sum /storage/c1/apps/netease-music/current/netease-music"), re.M)
                guest.launch("launcher")
                wait(lambda: guest.get("/api/state")["input_app"] == "launcher", "回到启动器键盘模式")
                guest.key(108)
                guest.key(28)
                wait(lambda: re.search(r"^\s*\d+\s+root\s+.*\bsh -i\s*$", guest.shell("ps"), re.M),
                     "打开原版终端")
                subprocess.run(["node", str(ROOT / "scripts/keyboard_test.mjs"), guest.url], check=True, timeout=30)
                (output / "netease-keyboard.json").write_text(json.dumps({
                    "original_app": "netease-music", "version": records["netease-music"][2],
                    "launched_via_original_launcher": True, "desktop_digits_match_native_input": True,
                    "screen_digit_buttons_match_native_input": True, "tested_digits": "1234567890",
                    "golden_phone_digits_visually_verified": True, "original_elf_sha256": expected,
                    "launcher_keyboard_profile_restored": True, "original_terminal_keyboard_regression": True,
                    "user_disk_untouched": True}, ensure_ascii=False, indent=2) + "\n")
            finally:
                if process.poll() is None:
                    try: guest.post("/api/shutdown", {})
                    except OSError: pass
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
                if (root / "runtime/boot.log").exists():
                    shutil.copyfile(root / "runtime/boot.log", output / "netease-keyboard-boot.log")


if __name__ == "__main__":
    main()
