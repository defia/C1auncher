#!/usr/bin/env python3
"""Embed a built Bash module without adding a core-update protocol component."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
data = args.input.read_bytes()
if not data.startswith(b"\x7fELF") or not 0 < len(data) <= 256 * 1024:
    raise SystemExit("idle module must be a bounded ELF image")
lines = ["/* Generated from terminal_idle_bash.c; do not edit. */",
         "static const unsigned char c1_terminal_idle_image[] = {"]
for offset in range(0, len(data), 16):
    lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset+16]) + ",")
lines.extend(["};", ""])
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text("\n".join(lines), encoding="ascii")
