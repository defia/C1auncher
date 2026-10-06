"""按已固定的官方索引，通过 HTTPS 按需下载应用包并缓存。"""
import hashlib
import json
import pathlib
import re
import tempfile
import threading
import urllib.request


class RepositoryError(Exception):
    pass


def download(url, size):
    # The official server is in China. TLS uses the host's certificate checks.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=20) as response:
        data = response.read(size + 1)
    if len(data) != size:
        raise RepositoryError("上游应用包大小与官方索引不一致")
    return data


class Repository:
    def __init__(self, base, cache, selection=None, log=lambda message: None):
        self.base = pathlib.Path(base).resolve()
        self.cache = pathlib.Path(cache).resolve()
        self.log = log
        self.packages = {}
        self.metadata = {}
        self.locks = {}
        self.url = ""
        if selection is None:
            return  # Legacy images still serve their existing local files.
        repository = selection["repository"]
        self.url = repository["url"].rstrip("/")
        if not self.url.startswith("https://"):
            raise RepositoryError("官方应用下载地址必须使用 HTTPS")
        self.metadata = {"index.v1": repository["index_sha256"],
                         "index.v1.sig": repository["signature_sha256"]}
        index = self._metadata("index.v1")
        self._metadata("index.v1.sig")
        for line in index.decode().splitlines():
            fields = line.split("\t")
            if fields[0] != "P":
                continue
            relative, sha256 = fields[4], fields[5]
            if not re.fullmatch(r"[0-9a-f]{64}", sha256) or relative != "objects/" + sha256 + ".tar.gz":
                raise RepositoryError("官方索引中的应用包路径不受支持")
            size = int(fields[6])
            if size <= 0:
                raise RepositoryError("官方索引中的应用包大小无效")
            self.packages[relative] = {"id": fields[1], "version": fields[2], "sha256": sha256, "size": size}
            self.locks[relative] = threading.Lock()

    def _metadata(self, relative):
        data = (self.base / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != self.metadata[relative]:
            raise RepositoryError("应用索引与固定版本不匹配，请重新组装镜像")
        return data

    @staticmethod
    def _valid(data, package):
        return len(data) == package["size"] and hashlib.sha256(data).hexdigest() == package["sha256"]

    def get(self, relative):
        target = (self.base / relative).resolve()
        if not target.is_relative_to(self.base):
            return None
        if relative in self.metadata:
            return self._metadata(relative)
        package = self.packages.get(relative)
        if package is None:
            # Local core updates, example media and audio fixtures remain usable.
            return target.read_bytes() if target.is_file() else None
        with self.locks[relative]:
            cached = (self.cache / relative).resolve()
            if not cached.is_relative_to(self.cache):
                raise RepositoryError("应用缓存路径不安全")
            for source in (target, cached):
                if source.is_file() and source.stat().st_size == package["size"]:
                    data = source.read_bytes()
                    if self._valid(data, package):
                        return data
            label = package["id"] + " " + package["version"]
            self.log("应用仓库：正在联网下载 " + label)
            try:
                data = download(self.url + "/" + relative, package["size"])
            except OSError as error:
                raise RepositoryError("应用下载失败：" + label + "；" + str(error)) from error
            if not self._valid(data, package):
                raise RepositoryError("应用包哈希与官方索引不一致：" + label)
            cached.parent.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=cached.parent, prefix=cached.name + ".", suffix=".part", delete=False) as stream:
                    temporary = pathlib.Path(stream.name)
                    stream.write(data)
                temporary.replace(cached)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            self.log("应用仓库：已校验并缓存 " + label)
            return data


def configured(root, log):
    root = pathlib.Path(root)
    selection = json.loads((root / "release.json").read_text()) if (root / "release.json").exists() else None
    return Repository(root / "build/repository", root / ".cache/channel", selection, log)
