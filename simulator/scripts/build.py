#!/usr/bin/env python3
"""使用 Docker 或 Podman 构建固定版本 MIPS 客体系统。"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", choices=["docker", "podman"])
    args = parser.parse_args()
    engine = args.engine
    if not engine:
        for candidate in ["podman", "docker"]:
            if shutil.which(candidate) and subprocess.run([candidate, "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                engine = candidate; break
    if not engine: parser.error("需要已启动的 Docker 或 Podman 环境")
    for script in ["fetch_sources.py", "fetch_release.py", "fetch_channel.py"]:
        subprocess.run([sys.executable, str(ROOT / "scripts" / script)], cwd=ROOT, check=True)
    image = "localhost/c1sim-builder:bookworm"
    subprocess.run([engine, "build", "-t", image, "-f", "guest/Dockerfile", "."], cwd=ROOT, check=True)
    subprocess.run([engine, "run", "--rm", "--name", "c1sim-build", "--volume", str(ROOT) + ":/work",
                    "--volume", "c1sim-build:/build", image, "sh", "guest/build.sh"], cwd=ROOT, check=True)
    print("构建完成。运行：python3 start.py")


if __name__ == "__main__": main()
