#!/usr/bin/env python3
"""校验官方签名更新；固定所选版本，保留原始 MIPS 二进制。"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache/channel"
SELECTED = ROOT / "release.json"
CORE_URL = "https://www.fwz233.com/c1/core/v1/stable"
APP_URL = "https://www.fwz233.com/c1/v2"
APP_IDS = ("hello", "book-reader", "pic", "chichugames", "music-player", "pinao")
INSTALLER = ROOT / ".cache/release-v2.0.0/C1SlimInstaller-2.0.0-lf-fix-20260907.zip"
INSTALLER_SHA256 = "ed00b36bc2d7616e5c133dd28df01d76d1b07404d6a7d686b1cffa4448cd28cf"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def keys():
    if digest(INSTALLER.read_bytes()) != INSTALLER_SHA256:
        raise ValueError("官方安装包哈希不匹配；请先执行 scripts/fetch_release.py")
    with zipfile.ZipFile(INSTALLER) as archive:
        def read(suffix):
            return archive.read(next(n for n in archive.namelist() if n.endswith(suffix)))
        return read("/enrollment/core.ed25519.pub"), read("/profile/repository.ed25519.pub")


def read(url, limit=1048576):
    # Official servers are in China; access them directly.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for attempt in range(3):
        try:
            with opener.open(url, timeout=30) as response:
                data = response.read(limit + 1)
            if len(data) > limit:
                raise ValueError("下载超过预期大小：" + url)
            return data
        except OSError:
            if attempt == 2:
                raise


def verify(message, signature, key):
    if len(key) != 32 or len(signature) != 64:
        raise ValueError("Ed25519 公钥或签名长度不正确")
    with tempfile.TemporaryDirectory(prefix="c1sim-signature-") as temporary:
        directory = pathlib.Path(temporary)
        (directory / "key.der").write_bytes(bytes.fromhex("302a300506032b6570032100") + key)
        (directory / "message").write_bytes(message)
        (directory / "signature").write_bytes(signature)
        result = subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-keyform", "DER",
                                 "-inkey", str(directory / "key.der"), "-rawin",
                                 "-in", str(directory / "message"), "-sigfile", str(directory / "signature")],
                                capture_output=True, text=True)
        if result.returncode:
            raise ValueError("官方 Ed25519 签名验证失败：" + result.stderr.strip())


def path(value):
    parts = pathlib.PurePosixPath(value).parts
    if not parts or value.startswith("/") or ".." in parts or not re.fullmatch(r"[A-Za-z0-9._/-]+", value):
        raise ValueError("更新包中的路径不安全：" + value)
    return value


def checked(url, target, expected, size=None):
    if target.is_file():
        data = target.read_bytes()
        if digest(data) == expected and (size is None or len(data) == size):
            return data
    data = read(url, size if size is not None else 1048576)
    if digest(data) != expected or (size is not None and len(data) != size):
        raise ValueError("更新文件的哈希或大小不匹配：" + url)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".part")
    temporary.write_bytes(data)
    temporary.replace(target)
    print("已校验下载：" + target.name, flush=True)
    return data


def latest(core_key, app_key):
    urls = [CORE_URL + "/manifest.v1", CORE_URL + "/manifest.v1.sig",
            APP_URL + "/index.v1", APP_URL + "/index.v1.sig"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        manifest, core_sig, index, app_sig = pool.map(read, urls)
    verify(manifest, core_sig, core_key)
    verify(index, app_sig, app_key)
    return describe(manifest, core_sig, index, app_sig), (manifest, core_sig, index, app_sig)


def describe(manifest, core_sig, index, app_sig):
    lines = manifest.decode().splitlines()
    if lines[0] != "C1CORE-MANIFEST 1":
        raise ValueError("不支持的核心清单格式")
    fields = [line.split("\t") for line in lines[1:]]
    values = {f[0]: f[1] for f in fields if f[0] != "F"}
    if values["T"] != "mips32r2-little-o32-hard-float-double-static":
        raise ValueError("核心 ABI 不兼容")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", values["V"]):
        raise ValueError("核心版本格式不正确")
    files = [{"id": f[1], "path": path(f[2]), "sha256": f[3], "size": int(f[4]), "mode": f[5]}
             for f in fields if f[0] == "F"]
    if {f["id"] for f in files} != {"c1ancher", "c1pkg", "launcher", "updater"}:
        raise ValueError("核心清单中的组件集合不兼容")
    lines = index.decode().splitlines()
    if lines[0] not in ["C1PKG-INDEX 1", "C1PKG-INDEX 2"]:
        raise ValueError("不支持的应用索引格式")
    records = {f[1]: f for line in lines[1:] if (f := line.split("\t"))[0] == "P"}
    apps = []
    for app_id in APP_IDS:
        f = records[app_id]
        apps.append({"id": app_id, "version": f[2], "name": f[3], "path": path(f[4]),
                     "sha256": f[5], "size": int(f[6]), "entry": path(f[7])})
    selection = {"schema": 1, "checked_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 "core": {"url": CORE_URL, "version": values["V"], "sequence": int(values["S"]),
                          "revision": values["R"], "manifest_sha256": digest(manifest),
                          "signature_sha256": digest(core_sig), "files": files},
                 "repository": {"url": APP_URL, "sequence": int(lines[1].split("\t")[1]),
                                "index_sha256": digest(index), "signature_sha256": digest(app_sig), "apps": apps}}
    return selection


def fetch(selection, core_key, app_key, envelopes=None):
    core = selection["core"]
    repository = selection["repository"]
    directory = CACHE / ("core-" + core["version"] + "-s" + str(core["sequence"]))
    index_dir = CACHE / ("index-" + str(repository["sequence"]))
    jobs = [(core["url"] + "/manifest.v1", directory / "manifest.v1", core["manifest_sha256"], None),
            (core["url"] + "/manifest.v1.sig", directory / "manifest.v1.sig", core["signature_sha256"], 64),
            (repository["url"] + "/index.v1", index_dir / "index.v1", repository["index_sha256"], None),
            (repository["url"] + "/index.v1.sig", index_dir / "index.v1.sig", repository["signature_sha256"], 64)]
    if envelopes:
        for job, data in zip(jobs, envelopes):
            target = job[1]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    messages = [checked(*job) for job in jobs]
    verify(messages[0], messages[1], core_key)
    verify(messages[2], messages[3], app_key)
    signed = describe(*messages)
    if selection["schema"] != 1 or core != signed["core"] or repository != signed["repository"]:
        raise ValueError("固定版本记录与官方签名清单不一致")
    jobs = [(core["url"] + "/" + f["path"], directory / f["path"], f["sha256"], f["size"])
            for f in core["files"]]
    # Build only needs bundled apps. Other signed packages are fetched by the
    # running simulator when the original package manager requests them.
    jobs += [(repository["url"] + "/" + app["path"], CACHE / app["path"], app["sha256"], app["size"])
             for app in repository["apps"]]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda job: checked(*job), jobs))
    print("官方核心和应用签名、文件哈希均通过。", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="只查询，不改变选定版本")
    group.add_argument("--update", action="store_true", help="下载验证并固定当前正式通道版本")
    args = parser.parse_args()
    core_key, app_key = keys()
    if args.check or args.update:
        selection, envelopes = latest(core_key, app_key)
        if SELECTED.exists():
            old = json.loads(SELECTED.read_text())
            if selection["core"]["sequence"] < old["core"]["sequence"] or selection["repository"]["sequence"] < old["repository"]["sequence"]:
                raise ValueError("官方通道序列回退，保留当前版本")
        print("核心：" + selection["core"]["version"] + "，序列 " + str(selection["core"]["sequence"]), flush=True)
        for app in selection["repository"]["apps"]:
            print(app["id"] + "：" + app["version"], flush=True)
        if args.check:
            return
        fetch(selection, core_key, app_key, envelopes)
        temporary = SELECTED.with_suffix(".json.part")
        temporary.write_text(json.dumps(selection, ensure_ascii=False, indent=2) + "\n")
        temporary.replace(SELECTED)
    elif SELECTED.exists():
        fetch(json.loads(SELECTED.read_text()), core_key, app_key)


if __name__ == "__main__":
    main()
