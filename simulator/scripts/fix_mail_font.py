#!/usr/bin/env python3
"""为模拟器里的 Mail 0.1.0 替换缺字字体，保留备份及原始 ELF。"""
import argparse
import hashlib
import io
import json
import pathlib
import secrets
import subprocess
import sys
import tarfile
import urllib.parse
import urllib.request

from import_netease_cookie import transfer_server

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from repository import configured, RepositoryError

FONT = "/storage/c1/apps/Mail/current/assets/ark-pixel-10px-zh_cn.ttf"
BACKUP = FONT + ".c1sim-original"
ORIGINAL_FONT_SHA256 = "88140095a350c1a3d9dc5729ad3686e88ba8db6ebaea3512df6281f298dc2005"


def materials():
    repository = configured(ROOT, lambda _: None)
    def package(app):
        relative, metadata = next((path, row) for path, row in repository.packages.items() if row["id"] == app)
        return metadata, tarfile.open(fileobj=io.BytesIO(repository.get(relative)))
    mail, archive = package("Mail")
    with archive:
        if mail["version"] != "0.1.0":
            raise ValueError("此工具仅用于已确认缺字的 Mail 0.1.0，请先检查新版应用。")
        original = archive.extractfile("payload/assets/ark-pixel-10px-zh_cn.ttf").read()
        if hashlib.sha256(original).hexdigest() != ORIGINAL_FONT_SHA256:
            raise ValueError("Mail 原包字体已变更，需要重新验证。")
        binary_sha256 = hashlib.sha256(archive.extractfile("payload/bin/mail").read()).hexdigest()
    _, archive = package("music-player")
    with archive:
        font = archive.extractfile("payload/assets/MiSans-Normal.ttf").read()
        license_text = archive.extractfile("payload/MiSans-LICENSE.txt").read()
    return font, license_text, binary_sha256


def fix(url="http://127.0.0.1:8765", restore=False):
    address = urllib.parse.urlsplit(url)
    if (address.scheme != "http" or address.hostname not in ("127.0.0.1", "localhost") or
        address.username or address.password or address.path not in ("", "/") or address.query or address.fragment):
        raise ValueError("--url 必须是本机模拟器的 HTTP 地址。")
    url = url.rstrip("/")
    with urllib.request.urlopen(url + "/api/state", timeout=5) as response:
        if not json.load(response).get("ready"):
            raise RuntimeError("请先启动模拟器。")
    font, license_text, binary_sha256 = materials()
    replacement_sha256 = hashlib.sha256(font).hexdigest()

    def execute(command):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/console.py"), "--url", url,
                                 "--timeout", "30", command], capture_output=True, timeout=40)
        if result.returncode:
            raise RuntimeError("字体操作未完成：请确认已安装原版 Mail 0.1.0，且其字体未被自行修改。")

    # Validate installed files before touching resources; account/mail storage
    # is separate and never read. No guest console output is echoed by this tool.
    guard = ("set -e; f=" + FONT + "; b=" + BACKUP + "; o=" + ORIGINAL_FONT_SHA256 + "; n=" + replacement_sha256 + "; "
             "test ! -L \"$f\"; test ! -L \"$b\"; "
             "test \"$(sha256sum /storage/c1/apps/Mail/current/bin/mail | awk '{print $1}')\" = " + binary_sha256 + "; "
             "c=$(sha256sum \"$f\" | awk '{print $1}'); ")
    if restore:
        execute("(" + guard + "test \"$c\" = \"$n\"; "
                "test \"$(sha256sum \"$b\" | awk '{print $1}')\" = \"$o\"; "
                "cp \"$b\" \"$f.new\"; mv -f \"$f.new\" \"$f\"; sync)")
        return
    bundle = io.BytesIO()
    with tarfile.open(fileobj=bundle, mode="w") as archive:
        for name, data in (("font.ttf", font), ("MiSans-LICENSE.txt", license_text)):
            member = tarfile.TarInfo(name); member.size = len(data); member.mode = 0o644
            archive.addfile(member, io.BytesIO(data))
    payload = bundle.getvalue()
    temporary = "/usr/data/c1/.mail-font." + secrets.token_hex(16)
    with transfer_server(payload) as download:
        command = ("(" + guard + "a=${f%/*}; if [ \"$c\" = \"$n\" ]; then "
                   "test \"$(sha256sum \"$b\" | awk '{print $1}')\" = \"$o\"; "
                   "test \"$(sha256sum \"$a/c1sim-MiSans-LICENSE.txt\" | awk '{print $1}')\" = " + hashlib.sha256(license_text).hexdigest() + "; exit 0; fi; "
                   "test \"$c\" = \"$o\"; if [ -e \"$b\" ]; then "
                   "test \"$(sha256sum \"$b\" | awk '{print $1}')\" = \"$o\"; else cp \"$f\" \"$b\"; fi; "
                   "t=" + temporary + "; mkdir \"$t\"; trap 'rm -f \"$t/bundle.tar\" \"$t/font.ttf\" \"$t/MiSans-LICENSE.txt\"; rmdir \"$t\"' EXIT; "
                   "wget -q -O \"$t/bundle.tar\" '" + download + "'; "
                   "test \"$(sha256sum \"$t/bundle.tar\" | awk '{print $1}')\" = " + hashlib.sha256(payload).hexdigest() + "; "
                   "tar -xf \"$t/bundle.tar\" -C \"$t\"; "
                   "cp \"$t/MiSans-LICENSE.txt\" \"$a/c1sim-MiSans-LICENSE.txt\"; "
                   "cp \"$t/font.ttf\" \"$f.new\"; chmod 644 \"$f.new\"; mv -f \"$f.new\" \"$f\"; sync)")
        execute(command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8765")
    parser.add_argument("--restore", action="store_true", help="恢复备份的官方原字体")
    args = parser.parse_args()
    try:
        fix(args.url, args.restore)
    except (OSError, ValueError, RuntimeError, RepositoryError, StopIteration, KeyError, tarfile.TarError,
            subprocess.TimeoutExpired) as error:
        parser.exit(1, "Mail 字体操作失败：" + str(error) + "\n")
    print("已恢复 Mail 原字体。" if args.restore else "已替换 Mail 缺字字体，原字体已备份，原始 ELF 保持不变。")
    print("请退出并重新打开 Mail。")


if __name__ == "__main__":
    main()
