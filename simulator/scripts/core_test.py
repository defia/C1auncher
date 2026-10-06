#!/usr/bin/env python3
"""在独立 QEMU 中验证本地核心、终端键盘和恢复官方核心。"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

from audio_test import Guest, wait
import pack_guest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-dir", type=pathlib.Path, default=ROOT.parent / "C1ancher/build")
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    developer = pack_guest.local_core(args.core_dir)
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    guest = Guest("http://127.0.0.1:" + str(args.port))
    results = []
    with tempfile.TemporaryDirectory(prefix="local-core-", dir=output) as temporary:
        root = pathlib.Path(temporary)
        shutil.copytree(ROOT / "build/images", root / "build/images")
        (root / ".cache").symlink_to(ROOT / ".cache", target_is_directory=True)
        (root / "public").symlink_to(ROOT / "public", target_is_directory=True)
        (root / "guest").symlink_to(ROOT / "guest", target_is_directory=True)
        shutil.copyfile(ROOT / "release.json", root / "release.json")

        def pack(core=None):
            with patch.multiple(pack_guest, ROOT=root, CACHE=root / ".cache/release-v2.0.0", BUILD=root / "build",
                                SELECTED=root / "release.json", CHANNEL=root / ".cache/channel"):
                pack_guest.main(core_dir=core)

        pack()
        official = json.loads((root / "build/binaries.json").read_text())
        before = hashlib.sha256((root / "build/images/rootfs.cpio.gz").read_bytes()).hexdigest()
        try:
            pack(root / "missing-core")
        except OSError:
            pass
        else:
            raise AssertionError("不完整核心未被拒绝")
        assert hashlib.sha256((root / "build/images/rootfs.cpio.gz").read_bytes()).hexdigest() == before
        results.append({"check": "reject-incomplete-core-without-changing-image", "passed": True})
        def run(local):
            # Redirect only this test subprocess's paths; exercise the real CLI.
            code = ("import pathlib,sys,start; start.ROOT=pathlib.Path(sys.argv.pop(1)).resolve(); "
                    "sys.path.insert(0, str(pathlib.Path(start.__file__).parent/'scripts')); "
                    "import pack_guest as p; p.ROOT=start.ROOT; p.BUILD=p.ROOT/'build'; "
                    "p.CACHE=p.ROOT/'.cache/release-v2.0.0'; p.SELECTED=p.ROOT/'release.json'; "
                    "p.CHANNEL=p.ROOT/'.cache/channel'; start.main()")
            command = [sys.executable, "-c", code, str(root), "--no-open", "--port", str(args.port),
                       "--audio-backend", "none"]
            if local:
                command += ["--core-dir", str(args.core_dir.resolve())]
            else:
                command += ["--disk", str(root / "runtime/dev-data.qcow2")]
            with (root / "runner.log").open("w") as log:
                process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                try:
                    wait(lambda: process.poll() is None and guest.get("/api/state")["ready"], "独立核心启动", 90)
                    guest.token = guest.get("/api/state")["token"]
                    assert (root / "runtime/dev-data.qcow2").is_file()
                    assert not (root / "runtime/data.qcow2").exists()
                    expected_rows = json.loads((root / "build/binaries.json").read_text())
                    for row in expected_rows:
                        text = guest.shell("sha256sum " + row["guest_path"])
                        assert re.search(r"^" + row["sha256"] + r"  " + re.escape(row["guest_path"]) + r"$", text, re.M), row
                    for row in official:
                        if row["guest_path"].startswith("/usr/data/c1/bin/"):
                            underlying = row["guest_path"].replace("/usr/data/", "/persist/usr-data/", 1)
                            assert re.search(r"^" + row["sha256"] + "  " + re.escape(underlying) + "$",
                                             guest.shell("sha256sum " + underlying), re.M), row
                    if local:
                        assert guest.get("/api/state")["apps"][0]["version"] == "local"
                        subprocess.run([sys.executable, str(ROOT / "scripts/smoke_test.py"), "--url", guest.url], check=True)
                        guest.launch("launcher")
                        guest.key(102)
                        time.sleep(.4)
                        guest.key(108)
                        guest.key(28)
                        wait(lambda: re.search(r"^\s*\d+\s+root\s+.*\bsh -i\s*$", guest.shell("ps"), re.M),
                             "打开原版终端")
                        subprocess.run(["node", str(ROOT / "scripts/keyboard_test.mjs"), guest.url], check=True, timeout=30)
                        guest.shell("printf local-core-persist > /usr/data/c1/.c1sim-test")
                    else:
                        assert "local-core-persist" in guest.shell("cat /usr/data/c1/.c1sim-test")
                        assert guest.get("/api/state")["apps"][0]["version"] != "local"
                    results.append({"check": "local-core-screen-keys-terminal-and-hashes" if local else
                                    "restore-official-core-and-preserve-data", "passed": True})
                finally:
                    if process.poll() is None:
                        try: guest.post("/api/shutdown", {})
                        except OSError: pass
                        try: process.wait(timeout=10)
                        except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
                    shutil.copyfile(root / "runtime/boot.log", output / ("local-core-boot.log" if local else "restored-core-boot.log"))

        run(True)
        pack()
        run(False)
        subprocess.run(["qemu-img", "check", str(root / "runtime/dev-data.qcow2")], check=True)
    (output / "core.json").write_text(json.dumps({"results": results,
        "local_binary_sha256": {name: info["sha256"] for name, (_, info) in developer.items()},
        "user_disk_untouched": True}, ensure_ascii=False, indent=2) + "\n")
    print("本地核心和官方恢复验证通过。", flush=True)


if __name__ == "__main__":
    main()
