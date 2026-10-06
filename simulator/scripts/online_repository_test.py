#!/usr/bin/env python3
"""用独立的真实 QEMU 验证商店首次联网安装及断网上游后的缓存重装。"""
import argparse
import hashlib
import io
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

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from audio_test import Guest, wait


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    selection = json.loads((ROOT / "release.json").read_text())
    records = {f[1]: f for line in (ROOT / "build/repository/index.v1").read_text().splitlines()
               if (f := line.split("\t"))[0] == "P"}
    app_id = "pelican"
    record = records[app_id]
    guest = Guest("http://127.0.0.1:" + str(args.port))
    with tempfile.TemporaryDirectory(prefix="online-", dir=output) as temporary:
        root = pathlib.Path(temporary)
        (root / "build/repository/objects").mkdir(parents=True)
        (root / "runtime").mkdir()
        (root / "public").symlink_to(ROOT / "public", target_is_directory=True)
        (root / "build/images").symlink_to(ROOT / "build/images", target_is_directory=True)
        shutil.copyfile(ROOT / "build/binaries.json", root / "build/binaries.json")
        shutil.copyfile(ROOT / "release.json", root / "release.json")
        for name in ("index.v1", "index.v1.sig"):
            shutil.copyfile(ROOT / "build/repository" / name, root / "build/repository" / name)
        for app in selection["repository"]["apps"]:
            os.link(ROOT / "build/repository" / app["path"], root / "build/repository" / app["path"])
        assert len(list((root / "build/repository/objects").iterdir())) == 6
        assert not (root / ".cache/channel" / record[4]).exists()
        assert not (root / "build/repository" / record[4]).exists()
        # Instrument only this disposable subprocess. Production download code
        # is used for the first request; subsequent upstream access is blocked.
        code = """import pathlib,sys,repository,start
start.ROOT=pathlib.Path(sys.argv.pop(1)).resolve()
original=repository.download
def download(url,size):
    counter=start.ROOT/'runtime/download-count'
    counter.write_text(str(int(counter.read_text())+1 if counter.exists() else 1))
    if (start.ROOT/'runtime/upstream-offline').exists(): raise OSError('test upstream unavailable')
    return original(url,size)
repository.download=download
start.main()
"""
        with (root / "runner.log").open("w") as log:
            process = subprocess.Popen([sys.executable, "-c", code, str(root), "--no-open", "--port", str(args.port),
                                        "--audio-backend", "none"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            try:
                def ready():
                    if process.poll() is not None:
                        raise AssertionError("独立模拟器提前退出：" + (root / "runner.log").read_text())
                    return guest.get("/api/state")["ready"]
                wait(ready, "独立 MIPS 客体启动", 90)
                guest.token = guest.get("/api/state")["token"]
                # Use the actual official shop: enter APP, switch to its shop,
                # select the previously uncached Pelican, confirm installation.
                time.sleep(.5)
                guest.key(28)
                time.sleep(.8)
                guest.key(106)
                time.sleep(.7)
                for _ in range(list(records).index(app_id)):
                    guest.key(108)
                    time.sleep(.05)
                guest.key(28)
                time.sleep(.15)
                guest.key(105)
                guest.key(28)
                entry = "/storage/c1/apps/" + app_id + "/current/" + record[7]
                wait(lambda: re.search(r"^C1_INSTALLED\r?$", guest.shell(
                     "if [ -f " + entry + " ]; then echo C1_INSTALLED; fi"), re.M),
                     "原版商店首次联网安装", 60)
                package = (root / ".cache/channel" / record[4]).read_bytes()
                assert len(package) == int(record[6]) and hashlib.sha256(package).hexdigest() == record[5]
                with tarfile.open(fileobj=io.BytesIO(package)) as archive:
                    expected = hashlib.sha256(archive.extractfile("payload/" + record[7]).read()).hexdigest()
                assert re.search("^" + expected + "  " + re.escape(entry) + "$", guest.shell("sha256sum " + entry), re.M)
                counter = root / "runtime/download-count"
                assert counter.read_text() == "1"
                subprocess.run([sys.executable, str(ROOT / "scripts/capture_frame.py"),
                                str(output / "online-store-installed.png"), "--url", guest.url], check=True)
                print("通过：空缓存 → 官方 HTTPS 下载 → 原版商店安装 → 原始 ELF 一致", flush=True)
                guest.post("/api/stop", {})
                (root / "runtime/upstream-offline").touch()
                guest.shell("c1pkg remove " + app_id)
                guest.shell("c1pkg install " + app_id, timeout=60)
                assert counter.read_text() == "1", "缓存重装不应再次访问上游"
                assert re.search("^" + expected + "  " + re.escape(entry) + "$", guest.shell("sha256sum " + entry), re.M)
                assert len(list((root / ".cache/channel/objects").iterdir())) == 1
                print("通过：阻断上游后，原版包管理器从缓存重装；其他商店包未下载", flush=True)
                report = {"app": app_id, "version": record[2], "source": selection["repository"]["url"],
                          "bundled_apps": 6, "newly_downloaded_apps": 1, "https_upstream_requests": 1,
                          "first_install_via_original_store": True, "offline_cached_reinstall": True,
                          "original_installed_elf_sha256": expected, "original_bytes_verified": True,
                          "other_store_apps_not_downloaded": True, "user_disk_untouched": True}
                (output / "online-repository.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            finally:
                if process.poll() is None:
                    try: guest.post("/api/shutdown", {})
                    except OSError: pass
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
                shutil.copyfile(root / "runner.log", output / "online-repository-runner.log")
                if (root / "runtime/boot.log").exists():
                    shutil.copyfile(root / "runtime/boot.log", output / "online-repository-boot.log")
                if process.returncode:
                    print((root / "runner.log").read_text(), file=sys.stderr)
    print("联网按需安装验证通过：" + str(output / "online-repository.json"))


if __name__ == "__main__":
    main()
