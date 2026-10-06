#!/usr/bin/env python3
"""检查真实商店仓库：官方签名索引中的每个下载必须可用且字节一致。"""
import argparse
import json
import pathlib
import urllib.error
import urllib.request

from fetch_channel import digest, keys, path, verify

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apps", nargs="*", help="只检查指定应用；默认检查整个商店")
    parser.add_argument("--url", default="http://127.0.0.1:8765")
    args = parser.parse_args()
    base = args.url.rstrip("/") + "/repo/"

    def get(relative, limit):
        with urllib.request.urlopen(base + path(relative), timeout=30) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise AssertionError("仓库返回文件超过预期大小：" + relative)
        return data

    index = get("index.v1", 1048576)
    verify(index, get("index.v1.sig", 64), keys()[1])
    lines = index.decode().splitlines()
    records = {f[1]: f for line in lines if (f := line.split("\t"))[0] == "P"}
    requested = args.apps or list(records)
    results = []
    for app_id in requested:
        f = records[app_id]
        try:
            data = get(f[4], int(f[6]))
        except urllib.error.HTTPError as error:
            raise AssertionError(f"商店包下载失败：{app_id} HTTP {error.code}") from error
        assert len(data) == int(f[6]) and digest(data) == f[5], "商店包哈希或大小不一致：" + app_id
        results.append({"id": app_id, "version": f[2], "size": len(data), "sha256": digest(data), "passed": True})
        print("已验证商店下载：" + app_id, flush=True)
    output = ROOT / "build/verification/repository.json"
    output.write_text(json.dumps({"index_sequence": int(lines[1].split("\t")[1]),
                                  "official_signature_verified": True, "results": results},
                                 ensure_ascii=False, indent=2) + "\n")
    print("商店仓库验证通过：" + str(output))


if __name__ == "__main__":
    main()
