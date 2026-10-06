#!/usr/bin/env python3
"""从本机文本文件导入网易云 Cookie，不将 Cookie 内容写入命令或日志。"""
import argparse
import contextlib
import hashlib
import http.server
import json
import pathlib
import re
import secrets
import subprocess
import sys
import threading
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = "/usr/data/c1/netease-music/cookie.txt"
MAX_BYTES = 65536


def read_cookie(path):
    with path.open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("Cookie 文件过大，请只保存 Cookie 请求头的值。")
    try:
        text = raw.decode("utf-8-sig").strip()
    except UnicodeDecodeError:
        raise ValueError("请将 Cookie 保存为 UTF-8 纯文本文件。") from None
    if not text or not text.isascii() or any(ord(c) < 32 or ord(c) == 127 for c in text):
        raise ValueError("Cookie 应是一行实际的请求头值，不能包含换行或控制字符。")
    if text.lower().startswith("cookie:"):
        raise ValueError("请删除 Cookie: 前缀，只保留请求头的值。")
    cookies = {}
    for part in text.split(";"):
        if not part.strip():
            continue
        name, separator, value = part.strip().partition("=")
        if not separator or not re.fullmatch(r"[!#$%&'*+\-.^_`|~0-9A-Za-z]+", name):
            raise ValueError("Cookie 格式错误，应为 名称=值; 名称=值。")
        cookies[name] = value
    if not cookies.get("MUSIC_U"):
        raise ValueError("文件缺少 MUSIC_U 登录凭据，请从已登录的网易云网页复制 Cookie。")
    return (text + "\n").encode("ascii")


@contextlib.contextmanager
def transfer_server(payload):
    route = "/" + secrets.token_hex(32)

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path != route:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

    # QEMU user networking reaches this loopback listener via 10.0.2.2.
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=.05), daemon=True)
    thread.start()
    try:
        yield f"http://10.0.2.2:{server.server_port}{route}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def import_cookie(path, url="http://127.0.0.1:8765"):
    address = urllib.parse.urlsplit(url)
    if (address.scheme != "http" or address.hostname not in ("127.0.0.1", "localhost")
            or address.username or address.password or address.path not in ("", "/")
            or address.query or address.fragment):
        raise ValueError("--url 必须是本机模拟器的 HTTP 地址。")
    url = url.rstrip("/")
    payload = read_cookie(path)
    with urllib.request.urlopen(url + "/api/state", timeout=5) as response:
        ready = json.load(response).get("ready")
    if not ready:
        raise RuntimeError("模拟器尚未就绪，请先启动模拟器。")
    digest = hashlib.sha256(payload).hexdigest()
    temporary = "/usr/data/c1/netease-music/.cookie-import." + secrets.token_hex(16)
    with transfer_server(payload) as download:
        # Only the short-lived transfer URL and a checksum enter the console;
        # the authentication cookie travels in the HTTP response body.
        command = ("(umask 077; mkdir -p /usr/data/c1/netease-music && "
                   "test ! -d " + TARGET + " && "
                   "mkdir " + temporary + " && (c1_cookie_tmp=" + temporary + "/cookie.txt; "
                   "(wget -q -O \"$c1_cookie_tmp\" '" + download + "' && "
                   "test \"$(wc -c < \"$c1_cookie_tmp\")\" -eq " + str(len(payload)) + " && "
                   "test \"$(sha256sum \"$c1_cookie_tmp\" | awk '{print $1}')\" = " + digest + " && "
                   "chmod 600 \"$c1_cookie_tmp\" && mv -f \"$c1_cookie_tmp\" " + TARGET + " && sync); "
                   "c1_cookie_rc=$?; rm -f \"$c1_cookie_tmp\"; rmdir " + temporary + "; "
                   "test \"$c1_cookie_rc\" -eq 0))")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/console.py"), "--url", url,
                                 "--timeout", "20", command], capture_output=True, timeout=30)
        if result.returncode:
            # Serial logs can contain unrelated account data: never echo them.
            raise RuntimeError("导入未完成，请检查模拟器是否正常运行后重试。")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=pathlib.Path, help="电脑上的 Cookie 纯文本文件路径")
    parser.add_argument("--url", default="http://127.0.0.1:8765", help="本机模拟器地址")
    args = parser.parse_args()
    try:
        import_cookie(args.file.expanduser(), args.url)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, "导入失败：" + str(error) + "\n")
    print("Cookie 已导入模拟器，文件权限为 600。请退出并重新打开网易云应用。")


if __name__ == "__main__":
    main()
