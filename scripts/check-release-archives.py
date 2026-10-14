#!/usr/bin/env python3
"""Fail closed when qev component archives omit their platform runtime contract."""

from __future__ import annotations

import argparse
import io
from pathlib import Path
import posixpath
import tarfile
import tempfile


class ArchiveError(ValueError):
    pass


class Archive:
    def __init__(self, path: Path):
        self.stream = tarfile.open(path, "r:gz")
        self.members: dict[str, tarfile.TarInfo] = {}
        try:
            for member in self.stream.getmembers():
                normalized = self._validated_name(member.name)
                if normalized in self.members:
                    raise ArchiveError(f"duplicate or colliding member name: {member.name!r}")
                if not (member.isdir() or member.isreg() or member.issym() or member.islnk()):
                    raise ArchiveError(f"unsupported member type: {member.name!r}")
                self._validated_link(member, normalized)
                self.members[normalized] = member
        except Exception:
            self.stream.close()
            raise

    @staticmethod
    def _normalize(name: str) -> str:
        return posixpath.normpath(name)

    @classmethod
    def _validated_name(cls, name: str) -> str:
        if not name or name.startswith(("/", "\\")) or "\\" in name \
           or (len(name) >= 2 and name[1] == ":"):
            raise ArchiveError(f"absolute or non-POSIX member name: {name!r}")
        raw_parts = name.split("/")
        if ".." in raw_parts:
            raise ArchiveError(f"parent traversal in member name: {name!r}")
        if any(part.startswith("._") or part == ".DS_Store" for part in raw_parts):
            raise ArchiveError(f"macOS metadata sidecar in component archive: {name!r}")
        normalized = cls._normalize(name)
        if normalized in {"", ".", ".."} or normalized.startswith("../"):
            raise ArchiveError(f"invalid member name: {name!r}")
        if normalized != name.rstrip("/"):
            raise ArchiveError(f"non-canonical member name: {name!r}")
        return normalized

    @staticmethod
    def _validated_link(member: tarfile.TarInfo, name: str) -> None:
        if not (member.issym() or member.islnk()):
            return
        target = member.linkname
        if not target or target.startswith(("/", "\\")) or "\\" in target \
           or (len(target) >= 2 and target[1] == ":") \
           or any(part in ("", ".", "..") for part in target.split("/")):
            raise ArchiveError(f"unsafe link target: {name!r} -> {target!r}")
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target)) \
            if member.issym() else posixpath.normpath(target)
        if resolved == ".." or resolved.startswith("../"):
            raise ArchiveError(f"link leaves component archive: {name!r} -> {target!r}")

    def close(self) -> None:
        self.stream.close()

    def find(self, suffix: str) -> tuple[str, tarfile.TarInfo] | None:
        wanted = tuple(self._normalize(suffix).split("/"))
        matches = [
            (name, member)
            for name, member in self.members.items()
            if tuple(name.split("/"))[-len(wanted) :] == wanted
        ]
        return matches[0] if len(matches) == 1 else None

    def bytes_of(self, member: tarfile.TarInfo) -> bytes | None:
        extracted = self.stream.extractfile(member)
        return extracted.read() if extracted is not None else None


def _regular_nonempty(archive: Archive, suffix: str) -> tuple[str, tarfile.TarInfo] | None:
    found = archive.find(suffix)
    if found is None or not found[1].isreg() or found[1].size <= 0:
        return None
    return found


def _valid_loader_alias(
    archive: Archive,
    alias_suffix: str,
    binary: tuple[str, tarfile.TarInfo],
) -> bool:
    alias = archive.find(alias_suffix)
    if alias is None:
        return False
    alias_name, alias_member = alias
    binary_name, binary_member = binary
    if alias_member.issym():
        if alias_member.linkname.startswith(("/", "\\")) or "\\" in alias_member.linkname \
           or ".." in alias_member.linkname.split("/"):
            return False
        return alias_member.linkname == posixpath.basename(binary_name)
    if alias_member.islnk():
        if alias_member.linkname.startswith(("/", "\\")) or "\\" in alias_member.linkname \
           or ".." in alias_member.linkname.split("/"):
            return False
        return Archive._normalize(alias_member.linkname) == binary_name
    if alias_member.isreg() and alias_member.size > 0:
        return archive.bytes_of(alias_member) == archive.bytes_of(binary_member)
    return False


def verify_runtime(archive: Archive, system: str, version: str) -> list[str]:
    major = version.split(".", 1)[0]
    key = system.lower()
    if key == "linux":
        binary_suffix = f"lib/libqev.so.{version}"
        alias_suffix = f"lib/libqev.so.{major}"
    elif key in {"darwin", "macos"}:
        binary_suffix = f"lib/libqev.{version}.dylib"
        alias_suffix = f"lib/libqev.{major}.dylib"
    elif key.startswith("win"):
        # Windows does not version DLL filenames; qev.dll is the loader name. The
        # component archive itself carries the release version in its filename.
        return [] if _regular_nonempty(archive, "bin/qev.dll") else ["runtime archive lacks a nonempty regular bin/qev.dll"]
    else:
        return [f"unsupported CPack system name: {system}"]
    binary = _regular_nonempty(archive, binary_suffix)
    errors: list[str] = []
    if binary is None:
        errors.append(f"runtime archive lacks a nonempty regular {binary_suffix}")
    elif not _valid_loader_alias(archive, alias_suffix, binary):
        errors.append(f"runtime archive lacks a valid {alias_suffix} alias to {binary_suffix}")
    return errors


def verify_directory(directory: Path, system: str, version: str) -> list[str]:
    runtime = directory / f"qev-{version}-{system}-qev_Runtime.tar.gz"
    development = directory / f"qev-{version}-{system}-qev_Development.tar.gz"
    errors: list[str] = []
    expected_names = {runtime.name, development.name}
    actual_names = {path.name for path in directory.glob(f"qev-{version}-{system}-*.tar.gz")}
    unexpected = sorted(actual_names - expected_names)
    if unexpected:
        errors.append(f"unexpected CPack archive basename(s): {', '.join(unexpected)}")
    if not runtime.is_file():
        errors.append(f"missing exact CPack archive basename: {runtime.name}")
    if not development.is_file():
        errors.append(f"missing exact CPack archive basename: {development.name}")
    if errors:
        return errors
    runtime_archive: Archive | None = None
    development_archive: Archive | None = None
    try:
        runtime_archive = Archive(runtime)
        development_archive = Archive(development)
    except (OSError, tarfile.TarError, ArchiveError) as exc:
        if runtime_archive is not None:
            runtime_archive.close()
        return [f"cannot read component archive: {exc}"]
    try:
        errors.extend(verify_runtime(runtime_archive, system, version))
        if _regular_nonempty(development_archive, "include/qev/ev.h") is None:
            errors.append("development archive lacks a nonempty regular include/qev/ev.h")
        for component, archive in (("Runtime", runtime_archive), ("Development", development_archive)):
            for notice in ("LICENSE", "THIRD-PARTY-NOTICES"):
                suffix = f"share/doc/qev/{notice}"
                if _regular_nonempty(archive, suffix) is None:
                    errors.append(f"{component} archive lacks a nonempty regular {suffix}")
    finally:
        runtime_archive.close()
        development_archive.close()
    return errors


def _write_archive(
    path: Path,
    regular: dict[str, bytes],
    symlinks: dict[str, str] | None = None,
    extra_regular: list[tuple[str, bytes]] | None = None,
    special: list[str] | None = None,
) -> None:
    with tarfile.open(path, "w:gz") as stream:
        for name, content in regular.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            stream.addfile(info, io.BytesIO(content))
        for name, target in (symlinks or {}).items():
            info = tarfile.TarInfo(name)
            info.type = tarfile.SYMTYPE
            info.linkname = target
            stream.addfile(info)
        for name, content in extra_regular or []:
            info = tarfile.TarInfo(name)
            info.size = len(content)
            stream.addfile(info, io.BytesIO(content))
        for name in special or []:
            info = tarfile.TarInfo(name)
            info.type = tarfile.FIFOTYPE
            stream.addfile(info)


def self_test() -> int:
    contracts: dict[str, tuple[str, str | None]] = {
        "Linux": ("lib/libqev.so.5.1.0", "lib/libqev.so.5"),
        "Darwin": ("lib/libqev.5.1.0.dylib", "lib/libqev.5.dylib"),
        "win64": ("bin/qev.dll", None),
    }
    for system, (binary_name, alias_name) in contracts.items():
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            runtime = directory / f"qev-5.1.0-{system}-qev_Runtime.tar.gz"
            development = directory / f"qev-5.1.0-{system}-qev_Development.tar.gz"
            notices = {
                "share/doc/qev/LICENSE": b"license",
                "share/doc/qev/THIRD-PARTY-NOTICES": b"notices",
            }
            _write_archive(development, {**notices, "include/qev/ev.h": b"header"})
            _write_archive(runtime, notices)
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} notices-only Runtime archive was accepted")
                return 1
            _write_archive(runtime, {**notices, binary_name: b""})
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} empty runtime binary was accepted")
                return 1
            aliases = {alias_name: posixpath.basename(binary_name)} if alias_name else {}
            _write_archive(runtime, {**notices, binary_name: b"binary"}, aliases)
            errors = verify_directory(directory, system, "5.1.0")
            if errors:
                print(f"FAIL: {system} valid fixture was refused: {'; '.join(errors)}")
                return 1
            _write_archive(runtime, {binary_name: b"binary", "share/doc/qev/THIRD-PARTY-NOTICES": b"notices"}, aliases)
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} Runtime archive without LICENSE was accepted")
                return 1
            _write_archive(runtime, {**notices, binary_name: b"binary", "share/doc/qev/LICENSE": b""}, aliases)
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} Runtime archive with empty LICENSE was accepted")
                return 1
            _write_archive(runtime, {**notices, binary_name: b"binary"}, aliases)
            _write_archive(development, {"include/qev/ev.h": b"header"})
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} Development archive without notices was accepted")
                return 1
            _write_archive(development, {**notices, "include/qev/ev.h": b"header", "share/doc/qev/LICENSE": b""})
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} Development archive with empty LICENSE was accepted")
                return 1
            _write_archive(development, {**notices, "include/qev/ev.h": b"header"})
            if alias_name:
                _write_archive(runtime, {**notices, binary_name: b"binary"})
                if not verify_directory(directory, system, "5.1.0"):
                    print(f"FAIL: {system} archive without its loader name was accepted")
                    return 1
                _write_archive(runtime, {**notices, binary_name: b"binary"}, {alias_name: "libqev-wrong"})
                if not verify_directory(directory, system, "5.1.0"):
                    print(f"FAIL: {system} archive with a wrong loader alias was accepted")
                    return 1
                # A regular copy is another valid CPack representation of the loader name,
                # but only when its bytes are the versioned binary's bytes.
                _write_archive(runtime, {**notices, binary_name: b"binary", alias_name: b"binary"})
                errors = verify_directory(directory, system, "5.1.0")
                if errors:
                    print(f"FAIL: {system} valid regular loader copy was refused: {'; '.join(errors)}")
                    return 1
                _write_archive(runtime, {**notices, binary_name: b"binary", alias_name: b"different"})
                if not verify_directory(directory, system, "5.1.0"):
                    print(f"FAIL: {system} unrelated regular loader file was accepted")
                    return 1
            fake_name = posixpath.join(posixpath.dirname(binary_name), "fake" + posixpath.basename(binary_name))
            _write_archive(runtime, {**notices, fake_name: b"binary"})
            if not verify_directory(directory, system, "5.1.0"):
                print(f"FAIL: {system} path without a segment boundary was accepted")
                return 1
            if system == "Linux":
                valid_regular = {**notices, binary_name: b"binary"}
                valid_links = {alias_name: posixpath.basename(binary_name)}
                malformed = [
                    ("parent traversal", "../lib/libqev.so.5.1.0", b"escape"),
                    ("absolute member", "/lib/libqev.so.5.1.0", b"escape"),
                    ("duplicate member", binary_name, b"second"),
                    ("normalized-name collision", "lib/./libqev.so.5.1.0", b"second"),
                    ("AppleDouble metadata", "lib/._libqev.so.5.1.0", b"sidecar"),
                ]
                for label, bad_name, content in malformed:
                    _write_archive(runtime, valid_regular, valid_links, [(bad_name, content)])
                    if not verify_directory(directory, system, "5.1.0"):
                        print(f"FAIL: {label} was accepted")
                        return 1
                _write_archive(runtime, valid_regular, valid_links, special=["lib/unsupported-fifo"])
                if not verify_directory(directory, system, "5.1.0"):
                    print("FAIL: special component member was accepted")
                    return 1
                for label, target in (("absolute link", "/tmp/outside"),
                                      ("traversing link", "../../../tmp/outside")):
                    _write_archive(runtime, valid_regular,
                                   {**valid_links, "share/doc/qev/escape": target})
                    if not verify_directory(directory, system, "5.1.0"):
                        print(f"FAIL: {label} was accepted")
                        return 1
                _write_archive(runtime, valid_regular, valid_links)
                wrong = directory / "qev-5.1.0-Linux-qev_Runtime-extra.tar.gz"
                _write_archive(wrong, valid_regular, valid_links)
                if not verify_directory(directory, system, "5.1.0"):
                    print("FAIL: wrong Runtime archive basename was accepted")
                    return 1
                wrong.unlink()
    print("release archive contract self-test: PASS (member type, size, path and loader alias controlled)")
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
    if args.directory is None or not args.system or not args.version:
        parser.error("--directory, --system and --version are required unless --self-test is used")
    errors = verify_directory(args.directory, args.system, args.version)
    for error in errors:
        print(f"FAIL: {error}")
    if errors:
        return 1
    print(f"release archives: PASS ({args.system} Runtime loader contract + Development header)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
