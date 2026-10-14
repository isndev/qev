#!/usr/bin/env python3
"""Remove macOS AppleDouble sidecars from CPack TGZ files before publication."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import re
import tarfile
import tempfile


def is_sidecar(name: str) -> bool:
    return any(part.startswith("._") or part == ".DS_Store"
               for part in PurePosixPath(name).parts)


def normalize(path: Path) -> tuple[int, int]:
    """Preserve every other tar member and atomically replace only a dirty archive."""
    with tarfile.open(path, "r:gz") as source:
        removed = sum(is_sidecar(member.name) for member in source)
    if not removed:
        return 0, 0

    descriptor, raw_temp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(raw_temp)
    kept = 0
    try:
        with tarfile.open(path, "r:gz") as source, os.fdopen(descriptor, "wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as target:
                    for member in source:
                        if is_sidecar(member.name):
                            continue
                        content = source.extractfile(member) if member.isfile() else None
                        target.addfile(member, content)
                        if content is not None:
                            content.close()
                        kept += 1
            raw.flush()
            os.fsync(raw.fileno())
        os.chmod(temporary, path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    return kept, removed


def _snapshot(path: Path) -> list[tuple]:
    with tarfile.open(path, "r:gz") as archive:
        return [
            (
                member.name, member.type, member.size, member.mode, member.uid, member.gid,
                member.uname, member.gname, member.mtime, member.linkname,
                member.devmajor, member.devminor, member.pax_headers,
                hashlib.sha256(archive.extractfile(member).read()).hexdigest() if member.isfile() else None,
            )
            for member in archive
        ]


def _fixture(path: Path) -> None:
    with tarfile.open(path, "w:gz") as archive:
        for name, payload in (
            ("lib/libqev.5.1.0.dylib", b"binary"),
            ("lib/._libqev.5.1.0.dylib", b"macOS metadata"),
            ("include/qev/ev.h", b"header"),
            ("include/qev/.DS_Store", b"finder metadata"),
            ("share/doc/qev/.DS_Storeful", b"legitimate retained content"),
        ):
            member = tarfile.TarInfo(name)
            member.size = len(payload)
            archive.addfile(member, io.BytesIO(payload))
        alias = tarfile.TarInfo("lib/libqev.5.dylib")
        alias.type = tarfile.SYMTYPE
        alias.linkname = "libqev.5.1.0.dylib"
        archive.addfile(alias)


def self_test() -> int:
    with tempfile.TemporaryDirectory() as raw:
        source = Path(raw) / "first.tar.gz"
        other = Path(raw) / "second.tar.gz"
        _fixture(source)
        other.write_bytes(source.read_bytes())
        retained = {
            "lib/libqev.5.1.0.dylib", "lib/libqev.5.dylib", "include/qev/ev.h",
            "share/doc/qev/.DS_Storeful",
        }
        before = [member for member in _snapshot(source) if member[0] in retained]
        if {member[0] for member in before} != retained:
            print("FAIL: fixture lacks one of the four retained members")
            return 1
        for path in (source, other):
            kept, removed = normalize(path)
            if (kept, removed) != (len(before), 2) or _snapshot(path) != before:
                print("FAIL: normalization altered a retained member or missed a sidecar")
                return 1
        if source.read_bytes() != other.read_bytes():
            print("FAIL: independent normalized archives are not byte-identical")
            return 1
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if normalize(source) != (0, 0) or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            print("FAIL: a clean archive was rewritten")
            return 1
    print("AppleDouble normalization self-test: PASS (metadata, links, bytes, determinism)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--system")
    parser.add_argument("--version")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.directory is None or args.system != "Darwin" \
       or not args.version or not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("--directory, --system Darwin and --version X.Y.Z are required")
    for component in ("Runtime", "Development"):
        archive = args.directory / f"qev-{args.version}-Darwin-qev_{component}.tar.gz"
        if not archive.is_file():
            print(f"FAIL: missing exact CPack archive {archive.name}")
            return 1
        kept, removed = normalize(archive)
        print(f"{archive.name}: kept={kept} AppleDouble-removed={removed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
