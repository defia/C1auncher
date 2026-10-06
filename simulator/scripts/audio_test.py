#!/usr/bin/env python3
"""用真实原版应用、MIPS 解码器和 QEMU 声卡输出验证音频；宿主 FFmpeg 仅生成输入样本。"""
import argparse
import array
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import wave

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Guest:
    def __init__(self, url):
        self.url = url
        self.token = None

    def get(self, path):
        with urllib.request.urlopen(self.url + path, timeout=5) as response:
            return json.load(response)

    def post(self, path, data):
        request = urllib.request.Request(self.url + path, json.dumps(data).encode(),
                                         {"Content-Type": "application/json", "X-C1Sim-Token": self.token})
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.load(response)

    def shell(self, command, timeout=15):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/console.py"), "--url", self.url,
                                 "--timeout", str(timeout), command], capture_output=True, text=True, timeout=timeout + 10)
        if result.returncode:
            raise AssertionError("客体命令失败：" + command + "\n" + result.stdout + result.stderr)
        return result.stdout

    def key(self, code, value=None):
        if value is None:
            self.post("/api/key", {"code": code, "value": 1})
            time.sleep(0.08)
            self.post("/api/key", {"code": code, "value": 0})
        else:
            self.post("/api/key", {"code": code, "value": value})

    def launch(self, app):
        before = self.get("/api/state")["sequence"]
        self.post("/api/run", {"id": app})
        wait(lambda: self.get("/api/state")["app"] == app and self.get("/api/state")["sequence"] > before,
             "启动 " + app)
        time.sleep(0.25)

    def pids(self, name):
        text = self.shell("for p in /proc/[0-9]*/comm; do read n < \"$p\"; "
                          f"if [ \"$n\" = {name} ]; then p=${{p%/comm}}; echo \"${{p#/proc/}}\"; fi; done")
        return re.findall(r"^(\d+)$", text, re.MULTILINE)


def wait(condition, description, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if condition(): return
        except OSError:
            pass
        time.sleep(0.1)
    raise AssertionError("等待超时：" + description)


def fixture(directory, ffmpeg):
    stereo = array.array("h")
    for i in range(44100 * 2):
        stereo.extend([int(7000 * math.sin(2 * math.pi * 440 * i / 44100)),
                       int(7000 * math.sin(2 * math.pi * 660 * i / 44100))])
    if sys.byteorder != "little": stereo.byteswap()
    (directory / "stereo.raw").write_bytes(stereo.tobytes())
    mono = bytearray()
    for i in range(32000 * 2):
        mono.extend((int(1800000 * math.sin(2 * math.pi * 880 * i / 32000)) & 0xffffff).to_bytes(3, "little"))
    (directory / "mono.raw").write_bytes(mono)
    for source, fmt, rate, channels, codec, name in [
        ("stereo.raw", "s16le", 44100, 2, "libmp3lame", "stereo.mp3"),
        ("mono.raw", "s24le", 32000, 1, "flac", "mono.flac"),
    ]:
        subprocess.run([ffmpeg, "-v", "error", "-f", fmt, "-ar", str(rate), "-ac", str(channels),
                        "-i", str(directory / source), "-c:a", codec, str(directory / name)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--base-disk", type=pathlib.Path, help="可选：复制已经关闭的测试磁盘，跳过首次应用安装")
    args = parser.parse_args()
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg: parser.error("音频集成测试需要宿主 ffmpeg 生成测试文件；日常运行不需要")
    output = ROOT / "build/verification"
    output.mkdir(parents=True, exist_ok=True)
    results = []
    segments = []
    # Use a disposable disk; never modify the user's normal simulator disk.
    with tempfile.TemporaryDirectory(prefix="audio-", dir=output) as temp:
        directory = pathlib.Path(temp)
        if args.base_disk: shutil.copyfile(args.base_disk, directory / "data.qcow2")
        fixture(directory, ffmpeg)
        repository = ROOT / "build/repository" / directory.name
        repository.mkdir()
        for name in ["stereo.mp3", "mono.flac", "stereo.raw"]:
            shutil.copyfile(directory / name, repository / name)
        recording = directory / "sound.wav"
        guest = Guest(f"http://127.0.0.1:{args.port}")
        log = (directory / "runner.log").open("w")
        process = subprocess.Popen([sys.executable, str(ROOT / "start.py"), "--no-open", "--port", str(args.port),
                                    "--disk", str(directory / "data.qcow2"), "--audio-record", str(recording)],
                                   stdout=log, stderr=subprocess.STDOUT)
        def position():
            time.sleep(0.08)
            return max(0, (recording.stat().st_size - 44) // 4)
        def segment(name, command):
            start = position()
            text = guest.shell(command)
            end = position()
            segments.append({"check": name, "start": start, "end": end})
            print("已执行：" + name, flush=True)
            return text
        ffplay = "/usr/bin/ffplay -nodisp -vn -autoexit -hide_banner -stats -- "
        mixer = """amixer -q cset 'numid=1,iface=MIXER,name=DAC Playback Volume' """
        try:
            wait(lambda: process.poll() is None and guest.get("/api/state")["ready"], "MIPS 客体就绪", 60)
            guest.token = guest.get("/api/state")["token"]
            guest.post("/api/stop", {})
            info = guest.shell("cat /proc/asound/cards; ls -l /dev/dsp /dev/mixer; ffplay -version")
            assert "ICH" in info and "FFmpeg 6.1.5" in info, info
            for name in ["stereo.mp3", "mono.flac", "stereo.raw"]:
                guest.shell(f"wget -q -O /tmp/{name} http://10.0.2.2:{args.port}/repo/{directory.name}/{name}")
            guest.shell(mixer + "158")
            for name, check in [("stereo.mp3", "mp3-44100-stereo"), ("mono.flac", "flac-32000-mono-24bit")]:
                text = segment(check, ffplay + "/tmp/" + name)
                assert re.search(r"Duration: 00:00:02[.]", text), text
                assert re.search(r"\s1[.]\d+ M-A:", text), text
                results.append({"check": check + "-duration-progress-exit", "passed": True})
            guest.shell(mixer + "0")
            segment("muted-mp3", ffplay + "/tmp/stereo.mp3")
            guest.shell(mixer + "79")
            segment("half-volume-mp3", ffplay + "/tmp/stereo.mp3")
            guest.shell(mixer + "158")
            guest.shell("(" + ffplay + "/tmp/stereo.mp3 >/tmp/io-audio.log 2>&1) & c1audio_pid=$!; "
                        "c1io_result=0; for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do "
                        "printf audio-io > /storage/mtp/Music/.c1sim-io-test || { c1io_result=1; break; }; "
                        "sync || { c1io_result=1; break; }; "
                        "test \"$(cat /storage/mtp/Music/.c1sim-io-test)\" = audio-io || { c1io_result=1; break; }; done; "
                        "wait \"$c1audio_pid\" || c1io_result=1; rm /storage/mtp/Music/.c1sim-io-test; test \"$c1io_result\" = 0")
            results.append({"check": "simultaneous-audio-and-disk-io", "passed": True})
            guest.shell("printf invalid > /tmp/invalid.mp3; " + ffplay +
                        "/tmp/invalid.mp3 >/tmp/invalid.log 2>&1; test $? -ne 0")
            results.append({"check": "invalid-file-fails", "passed": True})

            # Exercise the official Go player using only virtual evdev events.
            # Isolate the two-track playlist on this disposable disk, including
            # when --base-disk contains the user's existing music collection.
            guest.shell("mv /storage/mtp/Music /tmp/c1sim-original-Music; "
                        "mkdir -p /storage/mtp/Music; cp /tmp/stereo.mp3 /storage/mtp/Music/a.mp3; "
                        "cp /tmp/mono.flac /storage/mtp/Music/b.flac")
            guest.launch("music-player")
            start = position()
            guest.key(28)
            time.sleep(0.45)
            pids = guest.pids("ffplay")
            assert len(pids) == 1, pids
            child = pids[0]
            guest.key(28)
            time.sleep(0.12)
            paused = guest.shell(f"cat /proc/{child}/status")
            assert re.search(r"State:\s+T", paused), paused
            pause_start = position()
            time.sleep(0.25)
            segments.append({"check": "original-player-paused-output", "start": pause_start, "end": position()})
            results.append({"check": "original-player-pause", "passed": True})
            guest.key(28)
            time.sleep(0.15)
            resumed = guest.shell(f"cat /proc/{child}/status")
            assert not re.search(r"State:\s+T", resumed), resumed
            results.append({"check": "original-player-resume", "passed": True})
            volume_before = guest.shell("cat /run/c1sim-volume")
            guest.key(114)
            volume_after = guest.shell("cat /run/c1sim-volume")
            assert re.findall(r"^(\d+)$", volume_before, re.MULTILINE) != re.findall(r"^(\d+)$", volume_after, re.MULTILINE)
            results.append({"check": "original-player-volume-key", "passed": True})
            guest.key(106)
            time.sleep(0.25)
            track = guest.shell("ps w | grep '[f]fplay'")
            assert "/storage/mtp/Music/b.flac" in track, track
            results.append({"check": "original-player-next-track", "passed": True})
            time.sleep(2.3)
            track = guest.shell("ps w | grep '[f]fplay'")
            assert "/storage/mtp/Music/a.mp3" in track, track
            results.append({"check": "original-player-auto-next-at-eof", "passed": True})
            guest.post("/api/stop", {})
            time.sleep(0.5)
            assert not guest.pids("ffplay"), "退出后残留 ffplay 进程"
            segments.append({"check": "original-player-output", "start": start, "end": position()})
            results.append({"check": "original-player-stop-reaps-decoder", "passed": True})

            guest.shell("/sbin/c1sim-audio --init")
            guest.launch("pinao")
            start = position()
            guest.key(30, 1)
            time.sleep(0.7)
            guest.key(30, 0)
            time.sleep(0.2)
            guest.post("/api/stop", {})
            time.sleep(0.5)
            assert not guest.pids("aplay"), "退出后残留 aplay 进程"
            segments.append({"check": "original-pinao-output", "start": start, "end": position()})
            results.append({"check": "original-pinao-stop-reaps-aplay", "passed": True})
            guest.post("/api/shutdown", {})
            process.wait(timeout=10)
        finally:
            if process.poll() is None:
                try: guest.post("/api/shutdown", {})
                except OSError: pass
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=10)
            log.close()
            shutil.rmtree(repository)
            if recording.exists(): shutil.copyfile(recording, output / "audio.wav")
            shutil.copyfile(directory / "runner.log", output / "audio-runner.log")
            if process.returncode:
                print((directory / "runner.log").read_text(), file=sys.stderr)
        with wave.open(str(recording), "rb") as wav:
            assert wav.getframerate() == 48000 and wav.getnchannels() == 2 and wav.getsampwidth() == 2
            pcm = array.array("h", wav.readframes(wav.getnframes()))
        if sys.byteorder != "little": pcm.byteswap()
        for item in segments:
            samples = pcm[item["start"] * 2:item["end"] * 2]
            # QEMU buffers WAV fwrite, so file-size snapshots lag the sound
            # boundary by up to one block. Measure the settled middle region.
            settled = samples[9600:-9600] if len(samples) > 19200 else samples
            rms = math.sqrt(sum(float(value) ** 2 for value in settled) / max(1, len(settled)))
            item["rms"] = round(rms, 3)
            item["seconds"] = round(len(samples) / 96000, 3)
            if item["check"] in ("muted-mp3", "original-player-paused-output"): assert rms < 1, item
            else: assert rms > 100, item
            if item["check"] in ("mp3-44100-stereo", "flac-32000-mono-24bit"):
                assert 1.8 < item["seconds"] < 2.6, item
                for channel, expected in enumerate([440, 660] if item["check"].startswith("mp3") else [880, 880]):
                    values = samples[channel + 14400:channel + 62400:16]
                    def energy(freq):
                        real = sum(value * math.cos(2 * math.pi * freq * i / 6000) for i, value in enumerate(values))
                        imag = sum(value * math.sin(2 * math.pi * freq * i / 6000) for i, value in enumerate(values))
                        return real * real + imag * imag
                    assert energy(expected) > max(energy(freq) for freq in [220, 1100, 1500]) * 20, item
                item["channels_and_frequencies_verified"] = True
            item["passed"] = True
            results.append(item)
        full = next(row["rms"] for row in results if row["check"] == "mp3-44100-stereo")
        half = next(row["rms"] for row in results if row["check"] == "half-volume-mp3")
        assert 0 < half < full * 0.9, (half, full)
        shutil.copyfile(recording, output / "audio.wav")
    destination = output / "audio.json"
    destination.write_text(json.dumps({"decoder": "FFmpeg 6.1.5 in MIPS guest", "sound_card": "QEMU AC97",
                                      "results": results}, ensure_ascii=False, indent=2) + "\n")
    print("音频验证通过：" + str(destination), flush=True)


if __name__ == "__main__": main()
