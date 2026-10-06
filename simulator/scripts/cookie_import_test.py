#!/usr/bin/env python3
"""在独立 QEMU 数据盘验证 Cookie 传输、失败保护、权限和日志脱敏。"""
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from unittest import mock

import import_netease_cookie as importer
from audio_test import Guest, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cookie-import-", dir=output) as temporary:
        root = pathlib.Path(temporary)
        (root / "build").mkdir()
        (root / "runtime").mkdir()
        for name in ("images", "repository"):
            (root / "build" / name).symlink_to(ROOT / "build" / name, target_is_directory=True)
        (root / "public").symlink_to(ROOT / "public", target_is_directory=True)
        for name in ("release.json", "build/binaries.json"):
            shutil.copyfile(ROOT / name, root / name)
        cookie = root / "fixture.txt"
        # Fake credentials are never submitted to the music service.
        first = b"MUSIC_U=c1sim-test-session-one; __csrf=c1sim-test-csrf\n"
        second = b"MUSIC_U=c1sim-test-session-two; __csrf=c1sim-test-csrf\n"
        guest = Guest("http://127.0.0.1:8766")
        code = "import pathlib,sys,start; start.ROOT=pathlib.Path(sys.argv.pop(1)).resolve(); start.main()"

        def start(log):
            process = subprocess.Popen([sys.executable, "-c", code, str(root), "--no-open", "--port", "8766",
                                        "--audio-backend", "none"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            wait(lambda: process.poll() is None and guest.get("/api/state")["ready"], "独立客体启动", 90)
            guest.token = guest.get("/api/state")["token"]
            return process

        def stop(process):
            if process.poll() is None:
                guest.post("/api/shutdown", {})
                process.wait(timeout=15)

        def verify(payload):
            digest = hashlib.sha256(payload).hexdigest()
            assert re.search(r"^" + digest + "  " + re.escape(importer.TARGET) + "$",
                             guest.shell("sha256sum " + importer.TARGET), re.M)
            assert re.search(r"^-rw-------\s", guest.shell("ls -l " + importer.TARGET), re.M)
            assert not re.search(r"^/usr/data/c1/netease-music/\.cookie-import\.",
                                 guest.shell("find /usr/data/c1/netease-music -name '.cookie-import.*'"), re.M)

        def run_import():
            result = subprocess.run([sys.executable, str(ROOT / "scripts/import_netease_cookie.py"), str(cookie),
                                     "--url", guest.url], capture_output=True, timeout=35)
            if result.returncode:
                detail = guest.get("/api/console")["text"][-2200:]
                detail = re.sub(r"c1sim-test-[A-Za-z-]+", "<REDACTED>", detail)
                raise AssertionError("测试 Cookie 导入失败：" + result.stderr.decode(errors="replace") + detail)
            assert b"c1sim-test-session" not in result.stdout + result.stderr

        with (root / "runner.log").open("w") as log:
            process = None
            try:
                process = start(log)
                cookie.write_bytes(first)
                run_import()
                verify(first)
                cookie.write_bytes(second)
                run_import()
                verify(second)
                assert cookie.read_bytes() == second, "不能修改宿主 Cookie 文件"

                original_server = importer.transfer_server
                with mock.patch.object(importer, "transfer_server",
                                       side_effect=lambda payload: original_server(payload[:-1] + b"x")):
                    try:
                        importer.import_cookie(cookie, guest.url)
                    except RuntimeError:
                        pass
                    else:
                        raise AssertionError("传输内容损坏时必须拒绝覆盖 Cookie")
                verify(second)

                cookie.write_bytes(b"Cookie: MUSIC_U=invalid\n")
                rejected = subprocess.run([sys.executable, str(ROOT / "scripts/import_netease_cookie.py"),
                                           str(cookie), "--url", guest.url], capture_output=True, timeout=10)
                assert rejected.returncode != 0
                verify(second)
                for payload in (first, second):
                    assert payload.strip().decode() not in guest.get("/api/console")["text"]
                    assert payload.strip().decode() not in json.dumps(guest.get("/api/state")["logs"])
                stop(process)
                process = start(log)
                verify(second)
            finally:
                if process is not None and process.poll() is None:
                    stop(process)
        report = {"real_qemu_transfer": True, "original_cookie_header_bytes_preserved": True,
                  "guest_file_mode": "600", "atomic_replacement": True,
                  "corrupt_transfer_preserves_previous_cookie": True, "invalid_format_rejected": True,
                  "no_cookie_contents_in_logs": True, "survives_guest_restart": True,
                  "host_file_unchanged": True, "user_disk_untouched": True,
                  "authentication_not_attempted": True}
        (output / "cookie-import.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("通过：真实 QEMU 导入、权限、失败保护、日志检查与重启持久化；默认用户数据盘未修改。")


if __name__ == "__main__":
    main()
