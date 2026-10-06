#!/usr/bin/env python3
"""将当前原始电子纸 framebuffer 导出为 PNG，保留像素排列。"""
import argparse
import base64
import json
import pathlib
import struct
import urllib.request
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--url", default="http://127.0.0.1:8765")
    parser.add_argument("--scale", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.scale <= 8: parser.error("缩放范围为 1–8")
    with urllib.request.urlopen(args.url.rstrip("/") + "/api/frame", timeout=5) as response:
        frame = json.load(response)
    pixels = base64.b64decode(frame["pixels"])
    if len(pixels) != 5624 or frame["sequence"] < 2: raise SystemExit("应用尚未输出画面")
    rows = []
    for y in range(152):
        row = bytearray(b"\0")
        for x in range(296):
            rgb = bytes([36, 43, 36] if pixels[y // 8 * 296 + x] & (128 >> (y % 8)) else [241, 242, 233])
            row.extend(rgb * args.scale)
        rows.extend([bytes(row)] * args.scale)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    image = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 296 * args.scale, 152 * args.scale, 8, 2, 0, 0, 0))
    image += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"已保存客体帧 {frame['sequence']}：{args.output}")


if __name__ == "__main__": main()
