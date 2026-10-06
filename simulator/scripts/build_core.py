#!/usr/bin/env python3
"""用模拟器的 Linux 构建容器测试并交叉编译同仓库核心。"""
import argparse
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", choices=["docker", "podman"])
    args = parser.parse_args()
    engine = args.engine
    if not engine:
        for candidate in ("podman", "docker"):
            if shutil.which(candidate) and subprocess.run([candidate, "info"], stdout=subprocess.DEVNULL,
                                                         stderr=subprocess.DEVNULL).returncode == 0:
                engine = candidate
                break
    if not engine:
        parser.error("需要已启动的 Docker 或 Podman；先运行 scripts/build.py 构建客体和工具链")
    if not (ROOT.parent / "C1ancher/Makefile").is_file():
        parser.error("请在 C1auncher 仓库的 simulator/ 中运行")
    # The upstream Bash idle builtin is dlopened from /dev/shm. Container
    # defaults mount that directory noexec, which makes its host tests fail.
    subprocess.run([engine, "run", "--rm", "--pull=never", "--tmpfs", "/dev/shm:rw,exec,size=64m",
                    "--volume", str(ROOT.parent) + ":/src",
                    "--workdir", "/src/C1ancher", "localhost/c1sim-builder:bookworm",
                    "make", "-j2", "host-test", "all"], check=True)
    print("核心编译完成。运行：python3 start.py --core-dir ../C1ancher/build")


if __name__ == "__main__":
    main()
