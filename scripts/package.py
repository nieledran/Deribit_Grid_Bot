"""Package a deployed Windows runtime without private profiles or build artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
QT_VERSION = "6.8.3"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def qt_source(module):
    name = f"{module}-everywhere-src-{QT_VERSION}.tar.xz"
    cache = ROOT / ".deps" / "qt-sources"
    cache.mkdir(parents=True, exist_ok=True)
    source = cache / name
    checksum = cache / (name + ".sha256")
    url = f"https://download.qt.io/archive/qt/6.8/{QT_VERSION}/submodules/{name}"
    if not checksum.exists():
        print(f"Downloading {name}.sha256", flush=True)
        urllib.request.urlretrieve(url + ".sha256", checksum)
    expected = checksum.read_text(encoding="utf-8").split()[0].lower()
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise RuntimeError(f"Invalid upstream checksum for {name}")
    if not source.exists():
        print(f"Downloading {name}", flush=True)
        urllib.request.urlretrieve(url, source)
    if sha256(source) != expected:
        raise RuntimeError(f"Checksum mismatch: {source.name}; remove it and retry")
    return source


def check_executable(executable):
    data = executable.read_bytes()
    for value in (str(ROOT), ROOT.as_posix(), str(Path.home())):
        for encoding in ("utf-8", "utf-16le"):
            if value.encode(encoding).lower() in data.lower():
                raise RuntimeError("The EXE contains a local build/user path; rebuild Release first")
    # Catch other builders' Windows home paths and absolute debug-symbol references too.
    for text in (data.decode("latin1"), data.decode("utf-16le", errors="ignore")):
        if re.search(r"[A-Za-z]:[\\/]Users[\\/]|[A-Za-z]:[\\/][^\x00\r\n]*\.pdb", text,
                     re.IGNORECASE):
            raise RuntimeError("The EXE contains a user path or absolute PDB path")
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if data[:2] != b"MZ" or data[pe:pe + 6] != b"PE\x00\x00\x64\x86":
        raise RuntimeError("The release requires a Windows x64 executable")


def package(runtime, qt, output):
    required = ["DeribitGrid.exe", "qt.conf", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                "Qt6Network.dll", "Qt6WebSockets.dll", "Qt6Sql.dll", "msvcp140.dll",
                "vcruntime140.dll", "vcruntime140_1.dll", "plugins/platforms/qwindows.dll",
                "plugins/sqldrivers/qsqlite.dll", "plugins/tls/qschannelbackend.dll"]
    for name in required:
        if not (runtime / name).is_file():
            raise RuntimeError(f"Missing runtime file: {name}; deploy with scripts/build.py first")
    check_executable(runtime / "DeribitGrid.exe")
    config = (runtime / "qt.conf").read_text(encoding="utf-8")
    if re.search(r"[A-Za-z]:[\\/]|(?:^|=)\s*/", config, re.MULTILINE):
        raise RuntimeError("qt.conf must use relative paths")

    sboms = [qt / "sbom" / f"{module}-{QT_VERSION}.spdx.json"
             for module in ("qtbase", "qtwebsockets")]
    for path in sboms:
        if not path.is_file():
            raise RuntimeError(f"Qt {QT_VERSION} SBOM missing; use the pinned Qt installation")
        document = json.loads(path.read_text(encoding="utf-8"))
        if not any(p.get("versionInfo") == QT_VERSION for p in document.get("packages", [])):
            raise RuntimeError("Qt SBOM version does not match this release")
    for dll in runtime.glob("Qt6*.dll"):
        installed = qt / "bin" / dll.name
        if not installed.is_file() or sha256(installed) != sha256(dll):
            raise RuntimeError(f"{dll.name} differs from the Qt installation supplying the sources/SBOM")

    # Explicitly select runtime files: never recursively archive the working dist directory.
    files = {"DeribitGrid.exe": runtime / "DeribitGrid.exe", "qt.conf": runtime / "qt.conf"}
    for file in runtime.glob("*.dll"):
        files[file.name] = file
    for folder, suffix in (("plugins", ".dll"), ("translations", ".qm")):
        for file in (runtime / folder).rglob("*" + suffix):
            files[file.relative_to(runtime).as_posix()] = file
    for name in ("start.cmd", "README.md", "THIRD_PARTY.md", "LICENSE", "docs/BUILDING.md"):
        files[name] = ROOT / name
    for file in (ROOT / "licenses").rglob("*"):
        if file.is_file():
            files[file.relative_to(ROOT).as_posix()] = file
    for sbom in sboms:
        files["sbom/" + sbom.name] = sbom
    sources = [qt_source(module) for module in ("qtbase", "qtwebsockets")]
    for source in sources:
        files["sources/" + source.name] = source

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".zip.tmp")
    prefix = output.stem + "/"
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, file in sorted(files.items()):
                if file.is_symlink():
                    raise RuntimeError("Symlinks are not permitted in a portable release")
                compression = zipfile.ZIP_STORED if name.endswith(".tar.xz") else zipfile.ZIP_DEFLATED
                archive.write(file, prefix + name, compress_type=compression)
            for source in sources:
                module = source.name.split("-", 1)[0]
                with tarfile.open(source, "r:xz") as upstream:
                    for member in upstream:
                        parts = Path(member.name).parts
                        if member.isfile() and len(parts) == 3 and parts[1] == "LICENSES":
                            with upstream.extractfile(member) as license_file:
                                archive.writestr(prefix + f"licenses/Qt-{QT_VERSION}/{module}/" + parts[2],
                                                 license_file.read())
        with zipfile.ZipFile(temporary) as archive:
            if archive.testzip() is not None:
                raise RuntimeError("Release ZIP integrity check failed")
            if not any(name.endswith("/LGPL-3.0-only.txt") for name in archive.namelist()):
                raise RuntimeError("The Qt LGPL license text is missing")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    digest = sha256(output)
    output.with_suffix(".zip.sha256").write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(f"Portable release: {output}\nSHA-256: {digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt", type=Path,
                        default=Path(os.environ.get("QT_ROOT", ROOT / ".deps/Qt/6.8.3/msvc2022_64")))
    parser.add_argument("--dist", type=Path, default=ROOT / "dist")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "releases/DeribitGrid-0.1.0-windows-x64.zip")
    args = parser.parse_args()
    try:
        package(args.dist.resolve(), args.qt.resolve(), args.output.resolve())
    except (OSError, RuntimeError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        raise SystemExit(str(error))
