#!/usr/bin/env python3
"""用原版 Mail 和独立 QEMU 验证缺字字体替换、原始 ELF 与回滚。"""
import argparse
import base64
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

from audio_test import Guest, wait
from fix_mail_font import FONT, BACKUP, ORIGINAL_FONT_SHA256, materials

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    font, license_text, binary_sha256 = materials()
    font_sha256 = hashlib.sha256(font).hexdigest()
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    guest = Guest("http://127.0.0.1:" + str(args.port))
    with tempfile.TemporaryDirectory(prefix="mail-font-", dir=output) as temporary:
        root = pathlib.Path(temporary)
        (root / "build/repository/objects").mkdir(parents=True)
        (root / "public").symlink_to(ROOT / "public", target_is_directory=True)
        (root / "build/images").symlink_to(ROOT / "build/images", target_is_directory=True)
        for name in ("release.json", "build/binaries.json", "build/repository/index.v1", "build/repository/index.v1.sig"):
            shutil.copyfile(ROOT / name, root / name)
        selection = json.loads((ROOT / "release.json").read_text())
        apps = {app["id"] for app in selection["repository"]["apps"]} | {"Mail"}
        for line in (ROOT / "build/repository/index.v1").read_text().splitlines():
            record = line.split("\t")
            if record[0] != "P" or record[1] not in apps: continue
            source = ROOT / "build/repository" / record[4]
            if not source.exists(): source = ROOT / ".cache/channel" / record[4]
            os.link(source, root / "build/repository" / record[4])
        code = "import pathlib,sys,start; start.ROOT=pathlib.Path(sys.argv.pop(1)).resolve(); start.main()"
        with (root / "runner.log").open("w") as log:
            process = subprocess.Popen([sys.executable, "-c", code, str(root), "--no-open", "--port", str(args.port),
                                        "--audio-backend", "none"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait(lambda: process.poll() is None and guest.get("/api/state")["ready"], "独立 Mail 客体", 90)
                guest.token = guest.get("/api/state")["token"]
                guest.shell("c1pkg install Mail", timeout=60)
                installed = re.findall(r"^([A-Za-z0-9-]+)\t[0-9.]+$", guest.shell("c1pkg list"), re.M)

                def launch():
                    guest.launch("launcher")
                    guest.key(102); time.sleep(.4)
                    guest.key(28); time.sleep(.4)
                    for _ in range(installed.index("Mail")): guest.key(108)
                    guest.key(28)
                    wait(lambda: guest.get("/api/state")["input_app"] == "Mail", "原版启动器打开 Mail", 20)
                    # input_app reports exec before Go/font initialization has
                    # rendered. Empty Mail still has a footer in the final 24
                    # rows; the launcher's transient terminal does not.
                    wait(lambda: any(base64.b64decode(guest.get("/api/frame")["pixels"])[16 * 296:]),
                         "Mail 邮件列表与操作栏绘制", 30)
                    time.sleep(.5)

                def digest():
                    return hashlib.sha256(base64.b64decode(guest.get("/api/frame")["pixels"])).hexdigest()

                def font_tool(*extra):
                    subprocess.run([sys.executable, str(ROOT / "scripts/fix_mail_font.py"), "--url", guest.url, *extra], check=True)

                def capture(name):
                    subprocess.run([sys.executable, str(ROOT / "scripts/capture_frame.py"), str(output / name),
                                    "--url", guest.url], check=True)

                launch(); original_frame = digest(); capture("mail-font-before.png")
                assert ORIGINAL_FONT_SHA256 in guest.shell("sha256sum " + FONT)
                guest.launch("launcher"); font_tool(); font_tool()
                assert font_sha256 in guest.shell("sha256sum " + FONT)
                assert ORIGINAL_FONT_SHA256 in guest.shell("sha256sum " + BACKUP)
                assert binary_sha256 in guest.shell("sha256sum /storage/c1/apps/Mail/current/bin/mail")
                assert hashlib.sha256(license_text).hexdigest() in guest.shell(
                    "sha256sum /storage/c1/apps/Mail/current/assets/c1sim-MiSans-LICENSE.txt")
                launch(); fixed_frame = digest(); capture("mail-font-after.png")
                assert fixed_frame != original_frame, "字体替换未改变真实 Mail 画面"
                assert fixed_frame == "59778b595ecd0b0956a9e7cfc56791d2a5ab94421c3b82a1e2a18bedf2374062", \
                    "Mail 空列表画面与已目视确认的完整中文画面不一致"
                guest.launch("launcher"); font_tool("--restore")
                launch(); assert digest() == original_frame, "回滚没有恢复原始画面"
                assert binary_sha256 in guest.shell("sha256sum /storage/c1/apps/Mail/current/bin/mail")
                (output / "mail-font.json").write_text(json.dumps({"app": "Mail", "version": "0.1.0",
                    "original_elf_sha256": binary_sha256, "original_font_sha256": ORIGINAL_FONT_SHA256,
                    "replacement_font_sha256": font_sha256, "original_frame_sha256": original_frame,
                    "fixed_frame_sha256": fixed_frame, "idempotent": True, "license_preserved": True,
                    "original_elf_unchanged": True, "restore_matches_original_frame": True,
                    "user_disk_untouched": True}, ensure_ascii=False, indent=2) + "\n")
            finally:
                try:
                    state = guest.get("/api/state")
                    (output / "mail-font-state.json").write_text(json.dumps(state, ensure_ascii=False, indent=2))
                except OSError:
                    pass
                if process.poll() is None:
                    try: guest.post("/api/shutdown", {})
                    except OSError: pass
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
                if (root / "runtime/boot.log").exists():
                    shutil.copyfile(root / "runtime/boot.log", output / "mail-font-boot.log")
    print("Mail 字体替换、原始 ELF、许可及回滚验证通过。", flush=True)


if __name__ == "__main__":
    main()
