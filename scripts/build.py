"""Build-time helper only; the trading application never uses Python."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sqlite3
from datetime import datetime

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--qt", type=Path, default=root / ".deps/Qt/6.8.3/msvc2022_64")
parser.add_argument("--core-only", action="store_true")
parser.add_argument("--asan", action="store_true", help="Build and test with MSVC AddressSanitizer; never deploy")
parser.add_argument("--benchmarks", action="store_true", help="Include developer performance probes")
parser.add_argument("--skip-configure", action="store_true")
parser.add_argument("--skip-tests", action="store_true")
parser.add_argument("--skip-deploy", action="store_true")
args = parser.parse_args()

# Some Windows hosts supply both Path and PATH. MSBuild requires a unique key.
env = {k.upper(): v for k, v in os.environ.items()}
env["PATH"] = str(args.qt / "bin") + os.pathsep + env.get("PATH", "")
cmake = shutil.which("cmake", path=env["PATH"])
if not cmake:
    candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    if candidate.exists():
        cmake = str(candidate)
if not cmake:
    raise SystemExit("CMake was not found. Install Visual Studio C++/CMake tools.")
build = root / "build" / (("core" if args.core_only else "app") + ("-asan" if args.asan else ""))
if args.asan:
    compiler_root = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio/2022/Community/VC/Tools/MSVC"
    compiler_paths = sorted(compiler_root.glob("*/bin/Hostx64/x64"), key=lambda p: p.parts[-4])
    if compiler_paths:
        env["PATH"] = str(compiler_paths[-1]) + os.pathsep + env["PATH"]

def run(command):
    subprocess.run([str(item) for item in command], cwd=root, env=env, check=True)

if not args.skip_configure:
    command = [cmake, "-S", root, "-B", build, "-G", "Visual Studio 17 2022", "-A", "x64"]
    command.append("-DGRID_BUILD_BENCHMARKS=" + ("ON" if args.benchmarks else "OFF"))
    if args.asan:
        command.extend(["-DCMAKE_CXX_FLAGS=/fsanitize=address /Zi", "-DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO /DEBUG"])
    command.append("-DGRID_BUILD_GUI=" + ("OFF" if args.core_only else "ON"))
    if not args.core_only:
        if not (args.qt / "lib/cmake/Qt6/Qt6Config.cmake").exists():
            raise SystemExit("Qt 6 was not found. Specify --qt C:/path/to/msvc2022_64.")
        command.append("-DCMAKE_PREFIX_PATH=" + str(args.qt.resolve()))
    run(command)
run([cmake, "--build", build, "--config", "Release", "--parallel", "1"])
if not args.skip_tests:
    run([Path(cmake).with_name("ctest.exe"), "--test-dir", build, "-C", "Release", "--output-on-failure"])
if not args.core_only and not args.skip_deploy and not args.asan:
    processes = subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Process DeribitGrid -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id"], env=env, capture_output=True, text=True)
    if processes.stdout.strip():
        raise SystemExit("Close all DeribitGrid windows before updating dist. The tested build remains in " + str(build / "Release"))
    backup = root / "build/deployment-backups" / datetime.now().strftime("%Y%m%d-%H%M%S")
    backup.mkdir(parents=True, exist_ok=True)
    executable = root / "dist/DeribitGrid.exe"
    if executable.exists():
        shutil.copy2(executable, backup / executable.name)
    for profile in (root / "dist/profiles").glob("*.db"):
        with sqlite3.connect(profile.as_uri() + "?mode=ro", uri=True) as source, sqlite3.connect(backup / profile.name) as destination:
            source.backup(destination)
    print("Previous executable and consistent profile snapshots: " + str(backup))
    run([cmake, "--install", build, "--config", "Release", "--prefix", root / "dist"])
    redist = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio/2022/Community/VC/Redist/MSVC"
    crt_paths = sorted(redist.glob("*/x64/Microsoft.VC143.CRT"), key=lambda p: tuple(int(part) for part in p.parent.parent.name.split(".")))
    if crt_paths:
        for dll in crt_paths[-1].glob("*.dll"):
            shutil.copy2(dll, root / "dist" / dll.name)
    for name in ("start.cmd", "README.md", "THIRD_PARTY.md"):
        source = root / name
        if source.exists():
            shutil.copy2(source, root / "dist" / name)
    print("Application: " + str(root / "dist/DeribitGrid.exe"))
