"""Freeze an exclusive application stage with complete, offline-rebuildable source.

Run from any directory. Never signs, uploads, rewrites a prior stage or reads
credentials. Only explicitly allowlisted application/source files are included.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
APP = ROOT.name
FONT_SHA256 = "1a5f4112daaa9473747c6834041646cc9b2c338cb40ab5dbb2f0161f8968ca10"
MODULES = {"golang.org/x/image": "v0.45.0", "golang.org/x/sys": "v0.47.0", "golang.org/x/text": "v0.41.0"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--binary", type=Path, help="built ELF; defaults to versioned build/payload")
    parser.add_argument("--font", type=Path, help="music only: original MiSans Normal 4.003; never modified")
    parser.add_argument("--stage", type=Path, help="new, non-existing exclusive output directory")
    parser.add_argument("--go", default=shutil.which("go"), help="Go 1.26.4 executable")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("numeric semantic version required")
    if APP not in {"music-player", "book-reader"} or not args.go:
        parser.error("unsupported application or Go missing")
    build = REPO / "build" / f"{APP}-{args.version}"
    binary = args.binary or build / "payload" / APP
    files = {APP: binary.read_bytes(), "LICENSE": (REPO / "C1ancher/LICENSE").read_bytes()}
    for name in ("README.md", "VALIDATION.md", "THIRD_PARTY_NOTICES.md", "BUILDING.md"):
        files[name] = (ROOT / name).read_bytes()
    if APP == "music-player":
        if not args.font:
            parser.error("--font is required; no machine-specific default or substituted font")
        font = args.font.read_bytes()
        if hashlib.sha256(font).hexdigest() != FONT_SHA256:
            parser.error("original MiSans Normal 4.003 SHA256 mismatch")
        files["assets/MiSans-Normal.ttf"] = font
        for name in ("MiSans-LICENSE.pdf", "MiSans-LICENSE.txt", "MiSans-SOURCE.md"):
            files[name] = (ROOT / "licenses" / name).read_bytes()
    else:
        files["font-LICENSE.txt"] = (ROOT / "assets/font-LICENSE.txt").read_bytes()
        files["CHANGELOG.md"] = (ROOT / "CHANGELOG.md").read_bytes()
    source = {}
    for folder in (ROOT, REPO / "App/c1device"):
        for path in folder.rglob("*"):
            rel = path.relative_to(folder)
            if path.is_symlink():
                raise ValueError(f"source symlink rejected: {path}")
            if not path.is_file() or any(p in {"build", "assets", "__pycache__", ".git"} or p.startswith("build-") for p in rel.parts):
                continue
            if path.suffix in {".go", ".md", ".ps1", ".py"} or path.name in {"go.mod", "go.sum"} or (rel.parts[0] == "licenses" and path.suffix in {".txt", ".pdf"}):
                source[path.relative_to(REPO).as_posix()] = path.read_bytes()
    for path in (REPO / "LICENSE", REPO / "C1ancher/LICENSE"):
        source[path.relative_to(REPO).as_posix()] = path.read_bytes()
    if APP == "book-reader":
        for path in (ROOT / "assets/pkg-font.bin", ROOT / "assets/font-LICENSE.txt", REPO / "C1ancher/src/pkg/font_generated.h", REPO / "C1ancher/third_party/pkg_font/LICENSE.txt"):
            source[path.relative_to(REPO).as_posix()] = path.read_bytes()
    env = dict(os.environ, GOWORK="off", GOFLAGS="", GOTOOLCHAIN="local")
    output = subprocess.check_output([args.go, "list", "-deps", "-test", "-f", "{{with .Module}}{{.Path}} {{.Version}}{{end}}", "./..."], cwd=ROOT, env=env, text=True)
    external = dict(line.split()[:2] for line in output.splitlines() if line.startswith("golang.org/"))
    if external != MODULES:
        raise ValueError(f"dependency set changed; review before release: {external}")
    # go mod download checks the exact selected module ZIP against go.sum.
    # c1device is a local replacement, not a downloadable module ZIP.
    goroot = Path(subprocess.check_output([args.go, "env", "GOROOT"], text=True).strip())
    go_license = (goroot / "LICENSE").read_bytes()
    source["third-party/licenses/Go-LICENSE.txt"] = go_license
    files["licenses/Go-LICENSE.txt"] = go_license
    dependencies = []
    for module, version in MODULES.items():
        metadata = json.loads(subprocess.check_output([args.go, "mod", "download", "-json", f"{module}@{version}"], cwd=ROOT, env=env, text=True))
        for suffix, key in (("", "Sum"), ("/go.mod", "GoModSum")):
            if f"{module} {version}{suffix} {metadata[key]}" not in (ROOT / "go.sum").read_text():
                raise ValueError(f"dependency checksum missing or changed: {module}")
        for key, extension in (("Zip", "zip"), ("GoMod", "mod"), ("Info", "info")):
            data = Path(metadata[key]).read_bytes()
            name = f"third-party/goproxy/{module}/@v/{version}.{extension}"
            source[name] = data
        module_dir = Path(metadata["Dir"])
        if (module_dir / "LICENSE").read_bytes() != go_license:
            raise ValueError(f"license changed; review {module}")
        for name in ("LICENSE", "PATENTS"):
            path = module_dir / name
            if path.is_file():
                dest = f"licenses/{module.replace('/', '-')}-{name}.txt"
                files[dest] = path.read_bytes()
                source["third-party/" + dest] = path.read_bytes()
        dependencies.append({k: metadata[k] for k in ("Path", "Version", "Sum", "GoModSum")})
    source["third-party/dependencies.json"] = (json.dumps(dependencies, indent=2) + "\n").encode()
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for name, data in sorted(source.items()):
            info = tarfile.TarInfo("source/" + name)
            info.size, info.mode, info.mtime = len(data), 0o644, 0
            tar.addfile(info, io.BytesIO(data))
    files[f"{APP}-source-{args.version}.tar.gz"] = gzip.compress(raw.getvalue(), mtime=0)
    stage = args.stage or build / ("publish-" + time.strftime("%Y%m%d-%H%M%S") + "-license-reviewed")
    stage.mkdir(parents=True, exist_ok=False)
    report = []
    for name, data in sorted(files.items()):
        path = stage / "payload" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o755 if name == APP else 0o644)
        report.append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    (stage / "payload-hashes.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (stage / "source-files.json").write_text(json.dumps(sorted(source), indent=2) + "\n", encoding="utf-8")
    print(stage)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
