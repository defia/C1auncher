#!/usr/bin/env python3
"""组装 initramfs 和本地软件仓库；校验并保留官方二进制字节。"""
import gzip
import argparse
import hashlib
import io
import json
import pathlib
import struct
import tarfile
import zipfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "release-v2.0.0"
BUILD = ROOT / "build"
FILES = {}
AUDIT = []
SELECTED = ROOT / "release.json"
CHANNEL = ROOT / ".cache/channel"


def selection():
    return json.loads(SELECTED.read_text()) if SELECTED.exists() else None


def file(path, data, mode=0o644):
    FILES[path.lstrip("/")] = (0o100000 | mode, data)


def publish(target, data):
    """Atomically replace files also served by a running simulator."""
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".part")
    temporary.write_bytes(data)
    temporary.replace(target)


def elf(data, name):
    if data[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 8:
        raise ValueError("不是 ELF32 小端 MIPS：" + name)
    offset = struct.unpack_from("<I", data, 28)[0]
    entry_size, count = struct.unpack_from("<HH", data, 42)
    for i in range(count):
        if struct.unpack_from("<I", data, offset + entry_size * i)[0] == 3:
            raise ValueError("当前客体只支持静态二进制：" + name)
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "elf_flags": hex(struct.unpack_from("<I", data, 36)[0])}


def repository():
    repo = BUILD / "repository"
    (repo / "objects").mkdir(parents=True, exist_ok=True)
    release = selection()
    with zipfile.ZipFile(CACHE / "C1Slim-App-hello-1.5.1.zip") as archive:
        for name in ["index.v1", "index.v1.sig"]:
            if release:
                directory = CHANNEL / ("index-" + str(release["repository"]["sequence"]))
                data = (directory / name).read_bytes()
                expected = release["repository"]["index_sha256" if name == "index.v1" else "signature_sha256"]
                if hashlib.sha256(data).hexdigest() != expected: raise ValueError("签名索引与固定版本不匹配")
            else:
                data = archive.read("verification/" + name)
            publish(repo / name, data)
        key = archive.read("verification/repository.ed25519.pub")
        file("/usr/data/c1/pkg/repository.ed25519.pub", key)
    records = {fields[1]: fields for line in (repo / "index.v1").read_text().splitlines()
               if (fields := line.split("\t"))[0] == "P"}
    packages = []
    bundled = {app["id"] for app in release["repository"]["apps"]} if release else set(records)
    if release:
        package_ids = [app["id"] for app in release["repository"]["apps"]]
        for app_id in package_ids:
            record = records[app_id]
            path = CHANNEL / record[4]
            packages.append((path.name, path.read_bytes(), release["repository"]["url"] + "/" + record[4], record[1]))
    else:
        for path in sorted(CACHE.glob("C1Slim-App-*.zip")):
            with zipfile.ZipFile(path) as archive:
                name = next(name for name in archive.namelist() if name.endswith(".tar.gz"))
                packages.append((path.name, archive.read(name), "", None))
    for name, data, url, expected_id in packages:
        with tarfile.open(fileobj=io.BytesIO(data)) as package:
            manifest = package.extractfile("manifest.v1").read().decode()
            values = dict(line.split("\t", 1) for line in manifest.splitlines()[1:])
            if expected_id and values["id"] != expected_id:
                raise ValueError("应用包 ID 与签名索引不一致：" + name)
            record = records[values["id"]]
            if hashlib.sha256(data).hexdigest() != record[5] or len(data) != int(record[6]):
                raise ValueError("应用包与签名索引不一致：" + name)
            if values["version"] != record[2] or values["entry"] != record[7]:
                raise ValueError("应用清单与签名索引不一致：" + name)
            binary = package.extractfile("payload/" + values["entry"]).read()
            binary_info = elf(binary, values["id"])
            if values["id"] in bundled:
                AUDIT.append({"id": values["id"], "version": values["version"], "archive": name,
                              **({"url": url} if url else {}), "entry": values["entry"],
                              "guest_path": "/storage/c1/apps/" + values["id"] + "/current/" + values["entry"], **binary_info})
            if release:
                for member in package.getmembers():
                    if not member.isfile() or not ("licenses/" in member.name.lower() or any(word in member.name.rsplit("/", 1)[-1].upper() for word in ("LICENSE", "COPYING", "NOTICE", "PATENTS"))): continue
                    relative = pathlib.PurePosixPath(member.name)
                    if relative.is_absolute() or ".." in relative.parts: raise ValueError("不安全的许可证路径")
                    target = BUILD / "licenses/upstream" / (values["id"] + "-" + values["version"]) / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(package.extractfile(member).read())
        publish(repo / record[4], data)


def licenses():
    base = BUILD / "licenses/upstream"
    for source in sorted(CACHE.glob("*.zip")):
        with zipfile.ZipFile(source) as archive:
            for name in archive.namelist():
                if "licenses/" not in name or name.endswith("/"): continue
                relative = pathlib.PurePosixPath(name[name.index("licenses/"):])
                if ".." in relative.parts: raise ValueError("不安全的许可证路径")
                target = base / source.stem / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(name))


def originals():
    installer = next(CACHE.glob("C1SlimInstaller-*.zip"))
    release = selection()
    with zipfile.ZipFile(installer) as archive:
        root = archive.namelist()[0].split("/")[0] + "/payload/"
        core_key = archive.read(root + "enrollment/core.ed25519.pub")
        if release:
            directory = CHANNEL / ("core-" + release["core"]["version"] + "-s" + str(release["core"]["sequence"]))
            data = (directory / "manifest.v1").read_bytes()
            if hashlib.sha256(data).hexdigest() != release["core"]["manifest_sha256"]: raise ValueError("核心清单与固定版本不匹配")
            manifest = data.decode()
            signature = (directory / "manifest.v1.sig").read_bytes()
            if hashlib.sha256(signature).hexdigest() != release["core"]["signature_sha256"]: raise ValueError("核心签名与固定版本不匹配")
            version = next(line.split("\t")[1] for line in manifest.splitlines() if line.startswith("V\t"))
            if version != release["core"]["version"]: raise ValueError("核心版本与签名清单不一致")
        else:
            data = archive.read(root + "enrollment/release/manifest.v1")
            manifest = data.decode()
            signature = archive.read(root + "enrollment/release/manifest.v1.sig")
            version = "2.0.0"
        file("/opt/c1sim-core/manifest.v1", data)
        file("/opt/c1sim-core/manifest.v1.sig", signature)
        file("/opt/c1sim-core/core.ed25519.pub", core_key)
        for line in manifest.splitlines():
            fields = line.split("\t")
            if fields[0] != "F": continue
            data = (directory / fields[2]).read_bytes() if release else archive.read(root + "enrollment/release/" + fields[2])
            if hashlib.sha256(data).hexdigest() != fields[3] or len(data) != int(fields[4]):
                raise ValueError("核心二进制校验失败：" + fields[1])
            name = pathlib.PurePosixPath(fields[2]).name
            file("/usr/data/c1/bin/" + name, data, 0o755)
            file("/opt/c1sim-core/" + name, data, 0o755)
            AUDIT.append({"id": fields[1], "version": version, "archive": "manifest.v1" if release else installer.name,
                          **({"url": release["core"]["url"] + "/" + fields[2]} if release else {}),
                          "guest_path": "/usr/data/c1/bin/" + name, **elf(data, name)})
        repo = BUILD / "repository/core"
        for source, value in FILES.items():
            if source.startswith("opt/c1sim-core/"):
                relative = source[len("opt/c1sim-core/"):]
                target = repo / ("artifacts/" + relative if relative in ["C1ancher", "c1pkg", "C1ancher-launcher", "c1updater"] else relative)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(value[1])
        for name in ["neofetch", "c1-config.conf", "c1-logo.txt"]:
            file("/usr/data/c1/bin/" + name, archive.read(root + "accessories/" + name), 0o755 if name == "neofetch" else 0o644)
        file("/storage/mtp/Pic/wallpaper.raw", archive.read(root + "accessories/wallpaper.raw"))


def cpio():
    entries = dict(FILES)
    for name in list(entries):
        for parent in pathlib.PurePosixPath(name).parents:
            if str(parent) != ".": entries.setdefault(str(parent), (0o40755, b""))
    for name in ["dev", "proc", "sys", "tmp", "run", "root", "persist", "usr/bin", "usr/sbin", "etc"]:
        entries.setdefault(name, (0o40755, b""))
    output = io.BytesIO()
    def emit(name, mode, data, inode):
        encoded = name.encode() + b"\0"
        fields = [inode, mode, 0, 0, 2 if mode & 0o170000 == 0o40000 else 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
        if name == "dev/console": fields[9:11] = [5, 1]
        output.write(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
        output.write(encoded)
        output.write(b"\0" * (-output.tell() % 4))
        output.write(data)
        output.write(b"\0" * (-len(data) % 4))
    for index, (name, (mode, data)) in enumerate(sorted(entries.items()), 1): emit(name, mode, data, index)
    emit("TRAILER!!!", 0, b"", len(entries) + 1)
    with (BUILD / "images/rootfs.cpio.gz").open("wb") as stream:
        with gzip.GzipFile(fileobj=stream, mode="wb", mtime=0) as archive: archive.write(output.getvalue())


def local_core(directory):
    """Validate all four developer artifacts before changing the image."""
    if directory is None:
        return {}
    directory = pathlib.Path(directory).resolve()
    result = {}
    for name in ("C1ancher", "C1ancher-launcher", "c1pkg", "c1updater"):
        data = (directory / name).read_bytes()
        result[name] = (data, elf(data, name))
    return result


def main(binary=None, resources=None, core_dir=None):
    developer_core = local_core(core_dir)
    FILES.clear()
    AUDIT.clear()
    repository()
    originals()
    licenses()
    fingerprint = hashlib.sha256(json.dumps(AUDIT, sort_keys=True).encode()).hexdigest()
    file("/etc/c1sim-release", (fingerprint + "\n").encode())
    if developer_core:
        # Keep the official signed repository and persistent core unchanged.
        # /init overlays these files only after installing the official apps.
        file("/etc/c1sim-local-core", b"local-build\n")
        for name, (data, info) in developer_core.items():
            file("/opt/c1sim-dev-core/" + name, data, 0o755)
            row = next(row for row in AUDIT if row["guest_path"] == "/usr/data/c1/bin/" + name)
            app_id = row["id"]
            row.clear()
            row.update({"id": app_id, "version": "local", "source_type": "local-core",
                        "source": str(pathlib.Path(core_dir).resolve() / name),
                        "guest_path": "/usr/data/c1/bin/" + name, **info})
    for source, destination in [("busybox", "/bin/busybox"), ("c1sim.ko", "/lib/modules/c1sim.ko"), ("c1sim-bridge", "/sbin/c1sim-bridge"), ("c1sim-audio", "/sbin/c1sim-audio"), ("ffplay", "/usr/bin/ffplay"), ("curl", "/usr/bin/curl")]:
        data = (BUILD / "images" / source).read_bytes()
        if source != "c1sim.ko": elf(data, source)
        file(destination, data, 0o755)
    for applet in ["sh", "ash", "cat", "cp", "chmod", "chown", "date", "df", "du", "echo", "env", "false", "head", "id", "ln", "ls", "mkdir", "mknod", "mv", "pwd", "readlink", "realpath", "rm", "rmdir", "sha256sum", "sleep", "sort", "sync", "tail", "touch", "true", "uname", "wc", "whoami", "yes", "awk", "find", "grep", "sed", "xargs", "kill", "killall", "ps", "top", "free", "dmesg", "mount", "umount", "insmod", "lsmod", "halt", "poweroff", "reboot", "setsid", "cttyhack", "stty", "ifconfig", "route", "ip", "netstat", "nc", "wget", "tar", "gunzip", "gzip", "clear"]:
        FILES["bin/" + applet] = (0o120777, b"busybox")
    FILES["usr/bin/sha256sum"] = (0o120777, b"/bin/busybox")
    for name in ["aplay", "amixer"]: FILES["usr/bin/" + name] = (0o120777, b"/sbin/c1sim-audio")
    file("/init", (ROOT / "guest/init").read_bytes(), 0o755)
    FILES["dev/console"] = (0o20600, b"")
    file("/etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n")
    file("/etc/group", b"root:x:0:\n")
    file("/etc/hosts", b"127.0.0.1 localhost\n")
    file("/etc/ssl/certs/ca-certificates.crt", (BUILD / "images/ca-certificates.crt").read_bytes())
    file("/usr/data/c1/disable-auto-suspend", b"")
    file("/storage/mtp/Book/模拟器示例.txt", "C1-Slim MIPS 模拟器\n\n这是原始设备程序正在读取的示例文件。\n方向键翻页，Home 返回。\n".encode())
    # Self-contained sample image exercises the original image decoder.
    image = bytearray()
    for y in range(152):
        image.append(0)
        for x in range(296):
            black = (x - 148) ** 2 + (y - 76) ** 2 < 42 ** 2 or ((x // 8) % 2 == 0 and 125 <= y < 139 and 28 <= x < 268)
            image.append(0 if black else 255)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 296, 152, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(image)) + chunk(b"IEND", b"")
    file("/storage/mtp/Pic/sample.png", png)
    examples = BUILD / "repository/examples"
    examples.mkdir(exist_ok=True)
    (examples / "sample.png").write_bytes(png)
    if binary:
        binary = pathlib.Path(binary).resolve()
        data = binary.read_bytes()
        AUDIT.append({"id": "custom", "version": "local", "source": str(binary), "guest_path": "/opt/custom/app", **elf(data, str(binary))})
        if resources:
            resources = pathlib.Path(resources).resolve()
            if not resources.is_dir(): raise ValueError("资源目录不存在")
            for source in resources.rglob("*"):
                if source.is_symlink(): raise ValueError("资源目录中的符号链接需要先展开：" + str(source))
                if source.is_file(): file("/opt/custom/" + source.relative_to(resources).as_posix(), source.read_bytes(), 0o755 if source.stat().st_mode & 0o111 else 0o644)
        file("/opt/custom/app", data, 0o755)
    cpio()
    (BUILD / "binaries.json").write_text(json.dumps(AUDIT, ensure_ascii=False, indent=2) + "\n")
    print("客体镜像已组装；已核对 " + str(len(AUDIT)) + " 个原始 MIPS 二进制。")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path)
    parser.add_argument("--resources", type=pathlib.Path)
    parser.add_argument("--core-dir", type=pathlib.Path, help="含四个本地编译核心 ELF 的目录")
    args = parser.parse_args()
    if args.resources and not args.binary: parser.error("--resources 需要配合 --binary")
    main(args.binary, args.resources, args.core_dir)
