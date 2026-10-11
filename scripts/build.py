"""Build-time helper only; the trading application never uses Python."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--qt", type=Path, default=root / ".deps/Qt/6.8.3/msvc2022_64")
parser.add_argument("--core-only", action="store_true")
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
build = root / "build" / ("core" if args.core_only else "app")

def run(command):
    subprocess.run([str(item) for item in command], cwd=root, env=env, check=True)

if not args.skip_configure:
    command = [cmake, "-S", root, "-B", build, "-G", "Visual Studio 17 2022", "-A", "x64"]
    command.append("-DGRID_BUILD_GUI=" + ("OFF" if args.core_only else "ON"))
    if not args.core_only:
        if not (args.qt / "lib/cmake/Qt6/Qt6Config.cmake").exists():
            raise SystemExit("Qt 6 was not found. Specify --qt C:/path/to/msvc2022_64.")
        command.append("-DCMAKE_PREFIX_PATH=" + str(args.qt.resolve()))
    run(command)
run([cmake, "--build", build, "--config", "Release", "--parallel", "1"])
if not args.skip_tests:
    run([Path(cmake).with_name("ctest.exe"), "--test-dir", build, "-C", "Release", "--output-on-failure"])
if not args.core_only and not args.skip_deploy:
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
