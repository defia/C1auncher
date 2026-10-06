#!/usr/bin/env python3
"""在独立 QEMU 中验证网易云播放器参数、HTTP/HTTPS 流和 MIPS 音频输出。"""
import argparse
import array
import contextlib
import http.server
import json
import math
import pathlib
import re
import shlex
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import wave

from audio_test import Guest, fixture, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]


def certificates(root, openssl):
    def run(*args):
        subprocess.run([openssl, *args], cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=C1Sim test CA",
        "-keyout", "ca.key", "-out", "ca.pem")
    for name, address in (("server", "10.0.2.2"), ("wrong-host", "127.0.0.1")):
        run("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=" + address,
            "-keyout", name + ".key", "-out", name + ".csr")
        (root / (name + ".ext")).write_text("subjectAltName=IP:" + address + "\n")
        run("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
            "-days", "1", "-extfile", name + ".ext", "-out", name + ".pem")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    ffmpeg, openssl = shutil.which("ffmpeg"), shutil.which("openssl")
    if not ffmpeg or not openssl:
        parser.error("测试需要宿主 ffmpeg 和 openssl；日常播放不需要")
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="online-audio-", dir=output) as temporary, contextlib.ExitStack() as stack:
        root = pathlib.Path(temporary)
        (root / "build").mkdir(); (root / "runtime").mkdir()
        for name in ("images", "repository"):
            (root / "build" / name).symlink_to(ROOT / "build" / name, target_is_directory=True)
        (root / "public").symlink_to(ROOT / "public", target_is_directory=True)
        for name in ("release.json", "build/binaries.json"):
            shutil.copyfile(ROOT / name, root / name)
        fixture(root, ffmpeg)
        subprocess.run([ffmpeg, "-v", "error", "-f", "s16le", "-ar", "44100", "-ac", "2", "-i",
                        str(root / "stereo.raw"), "-c:a", "aac", str(root / "stereo.m4a")], check=True)
        certificates(root, openssl)
        requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path == "/ca.pem":
                    payload = (root / "ca.pem").read_bytes()
                elif self.path == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", f"https://10.0.2.2:{tls.server_port}/stereo.mp3")
                    self.end_headers()
                    return
                elif self.path in ("/stereo.mp3", "/mono.flac", "/stereo.m4a"):
                    if self.headers.get("User-Agent") != "Mozilla/5.0" or self.headers.get("Referer") != "https://music.163.com":
                        self.send_error(403)
                        return
                    requests.append(self.path)
                    payload = (root / self.path[1:]).read_bytes()
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        def server(cert=None):
            result = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            result.daemon_threads = True
            if cert:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(root / (cert + ".pem"), root / (cert + ".key"))
                result.socket = context.wrap_socket(result.socket, server_side=True)
            thread = threading.Thread(target=lambda: result.serve_forever(poll_interval=.05), daemon=True)
            thread.start()
            stack.callback(result.server_close); stack.callback(result.shutdown)
            return result

        plain, tls, wrong = server(), server("server"), server("wrong-host")
        recording = root / "sound.wav"
        guest = Guest("http://127.0.0.1:" + str(args.port))
        code = "import pathlib,sys,start; start.ROOT=pathlib.Path(sys.argv.pop(1)).resolve(); start.main()"
        segments = {}
        with (root / "runner.log").open("w") as log:
            process = subprocess.Popen([sys.executable, "-c", code, str(root), "--no-open", "--port", str(args.port),
                                        "--audio-record", str(recording)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait(lambda: process.poll() is None and guest.get("/api/state")["ready"], "独立客体启动", 90)
                guest.token = guest.get("/api/state")["token"]
                guest.post("/api/stop", {})
                guest.shell("amixer -q cset 'numid=1,iface=MIXER,name=DAC Playback Volume' 158")
                # Build CRLF inside the guest: a serial tty converts literal CR
                # to LF. The sentinel preserves the trailing newline in $(...).
                player = ("c1_audio_headers=$(printf 'User-Agent: Mozilla/5.0\\r\\nReferer: https://music.163.com\\r\\n.'); "
                          "c1_audio_headers=${c1_audio_headers%.}; "
                          "ffplay -nodisp -vn -autoexit -hide_banner -stats -headers \"$c1_audio_headers\"")

                def play(name, address, gain="1", success=True):
                    time.sleep(.1)
                    start = max(0, (recording.stat().st_size - 44) // 4)
                    command = player + " -af volume=" + gain + " -- " + shlex.quote(address)
                    text = guest.shell(command + "; echo ONLINE_AUDIO_RESULT=$?", timeout=25)
                    time.sleep(.1)
                    end = max(0, (recording.stat().st_size - 44) // 4)
                    result = re.search(r"^ONLINE_AUDIO_RESULT=(\d+)$", text, re.M)
                    assert result and (int(result[1]) == 0) == success, name + " 未达到预期：\n" + text
                    if success:
                        assert re.search(r"Duration: 00:00:02[.]", text), name + " 未读取音频时长"
                        assert re.search(r"\s1[.]\d+ M-A:", text), name + " 未输出播放进度"
                        segments[name] = (start, end)
                    print("通过：" + name, flush=True)

                base = f"http://10.0.2.2:{plain.server_port}"
                secure = f"https://10.0.2.2:{tls.server_port}"
                play("http-mp3", base + "/stereo.mp3")
                play("http-volume-half", base + "/stereo.mp3", "0.5")
                play("http-volume-zero", base + "/stereo.mp3", "0")
                play("untrusted-tls-rejected", secure + "/stereo.mp3", success=False)
                guest.shell("mkdir -p /etc/ssl/certs; wget -q -O /tmp/test-ca.pem " + base +
                            "/ca.pem && cat /tmp/test-ca.pem >> /etc/ssl/certs/ca-certificates.crt")
                play("https-flac", secure + "/mono.flac")
                play("https-aac", secure + "/stereo.m4a")
                play("http-to-https-redirect", base + "/redirect")
                play("wrong-tls-host-rejected", f"https://10.0.2.2:{wrong.server_port}/stereo.mp3", success=False)
                play("http-404-rejected", base + "/missing", success=False)
                play("invalid-volume-rejected", base + "/stereo.mp3", "nan", success=False)
            finally:
                if process.poll() is None:
                    try: guest.post("/api/shutdown", {})
                    except OSError: pass
                    try: process.wait(timeout=15)
                    except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
        with wave.open(str(recording), "rb") as audio:
            assert audio.getframerate() == 48000 and audio.getnchannels() == 2 and audio.getsampwidth() == 2
            samples = array.array("h", audio.readframes(audio.getnframes()))
        if sys.byteorder != "little": samples.byteswap()
        def rms(name):
            start, end = segments[name]
            assert 1.8 < (end - start) / 48000 < 2.8, name + " 声卡输出长度不正确"
            # QEMU buffers WAV writes. A file-size snapshot can still include
            # the previous voice's tail; measure the settled middle, as in audio_test.py.
            selected = samples[(start + 4800) * 2:(end - 4800) * 2]
            assert selected, name + " 没有声卡样本"
            return math.sqrt(sum(value * value for value in selected) / len(selected))
        full, half, zero = (rms(name) for name in ("http-mp3", "http-volume-half", "http-volume-zero"))
        shutil.copyfile(recording, output / "online-audio.wav")
        measurements = {name: {"frames": segments[name], "rms": rms(name)} for name in segments}
        (output / "online-audio-measurements.json").write_text(json.dumps(measurements, indent=2) + "\n")
        assert full > 100 and .4 < half / full < .6 and zero < 1, \
            f"网络播放软件音量或 PCM 输出不正确：full={full}, half={half}, zero={zero}"
        for name in ("https-flac", "https-aac", "http-to-https-redirect"):
            assert rms(name) > 100, name + " 没有有效 PCM 音频"
        assert all(name in requests for name in ("/stereo.mp3", "/mono.flac", "/stereo.m4a"))
        report = {"real_mips_qemu_audio": True, "original_netease_cli_headers_and_volume": True,
                  "http_mp3": True, "https_flac": True, "https_aac": True, "http_to_https_redirect": True,
                  "untrusted_cert_rejected": True, "wrong_cert_host_rejected": True, "http_404_rejected": True,
                  "invalid_gain_rejected": True, "pcm_nonzero": True, "half_gain_rms_ratio": half / full,
                  "zero_gain_rms": zero, "user_disk_untouched": True, "no_account_credentials_used": True}
        (output / "online-audio.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("网络音频与原版网易云参数兼容验证通过。")


if __name__ == "__main__":
    main()
