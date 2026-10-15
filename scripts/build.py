"""Build-time helper only; the trading application never uses Python."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sqlite3
import hashlib
import zipfile
from datetime import datetime

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--qt", type=Path,
                    default=Path(os.environ.get("QT_ROOT", root / ".deps/Qt/6.8.3/msvc2022_64")))
parser.add_argument("--core-only", action="store_true")
parser.add_argument("--asan", action="store_true", help="Build and test with MSVC AddressSanitizer; never deploy")
parser.add_argument("--benchmarks", action="store_true", help="Include developer performance probes")
parser.add_argument("--skip-configure", action="store_true")
parser.add_argument("--skip-tests", action="store_true")
parser.add_argument("--skip-deploy", action="store_true")
parser.add_argument("--deploy-dir", type=Path, default=root / "dist",
                    help="Runtime destination; use a separate directory to prepare a public release")
args = parser.parse_args()
deployment = args.deploy_dir if args.deploy_dir.is_absolute() else root / args.deploy_dir
deployment = deployment.resolve()

# Some Windows hosts supply both Path and PATH. MSBuild requires a unique key.
env = {k.upper(): v for k, v in os.environ.items()}
env["PATH"] = str(args.qt / "bin") + os.pathsep + env.get("PATH", "")
cmake = shutil.which("cmake", path=env["PATH"])
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs_root = None
if vswhere.exists():
    found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-version", "[17.0,18.0)",
                            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True, check=True)
    if found.stdout.strip():
        vs_root = Path(found.stdout.strip())
if not cmake and vs_root:
    candidate = vs_root / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    if candidate.exists():
        cmake = str(candidate)
if not cmake:
    raise SystemExit("CMake was not found. Install Visual Studio C++/CMake tools.")
build = root / "build" / (("core" if args.core_only else "app") + ("-asan" if args.asan else ""))
if args.asan:
    if not vs_root:
        raise SystemExit("Visual Studio 2022 C++ tools were not found for AddressSanitizer.")
    compiler_root = vs_root / "VC/Tools/MSVC"
    compiler_paths = sorted(compiler_root.glob("*/bin/Hostx64/x64"),
                            key=lambda p: tuple(int(part) for part in p.parts[-4].split(".")))
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
    executable = deployment / "DeribitGrid.exe"
    processes = subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Process DeribitGrid* -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path"], env=env, capture_output=True, text=True)
    if any(os.path.normcase(path.strip()) == os.path.normcase(str(executable))
           for path in processes.stdout.splitlines() if path.strip()):
        raise SystemExit("Close DeribitGrid windows using this deployment before updating it. "
                         "Or choose a separate --deploy-dir. The tested build remains in " + str(build / "Release"))
    backup = root / "build/deployment-backups" / datetime.now().strftime("%Y%m%d-%H%M%S")
    backup.mkdir(parents=True, exist_ok=True)
    if executable.exists():
        with zipfile.ZipFile(backup / "previous-app.zip", "w", zipfile.ZIP_DEFLATED) as archive:
            archive.write(executable, executable.name)
    for profile in (deployment / "profiles").glob("*.db"):
        with sqlite3.connect(profile.as_uri() + "?mode=ro", uri=True) as source, sqlite3.connect(backup / profile.name) as destination:
            source.backup(destination)
    print("Previous executable and consistent profile snapshots: " + str(backup))
    run([cmake, "--install", build, "--config", "Release", "--prefix", deployment])
    built_hash = hashlib.sha256((build / "Release/DeribitGrid.exe").read_bytes()).digest()
    if hashlib.sha256(executable.read_bytes()).digest() != built_hash:
        raise SystemExit("Deployment verification failed: installed EXE differs from the tested build.")
    if not vs_root:
        raise SystemExit("Visual Studio redistributable DLLs were not found. No portable release was created.")
    redist = vs_root / "VC/Redist/MSVC"
    crt_paths = sorted(redist.glob("*/x64/Microsoft.VC143.CRT"), key=lambda p: tuple(int(part) for part in p.parent.parent.name.split(".")))
    if not crt_paths:
        raise SystemExit("Install the Visual Studio x64 C++ runtime redistributable components.")
    for dll in crt_paths[-1].glob("*.dll"):
        shutil.copy2(dll, deployment / dll.name)
    for name in ("start.cmd", "README.md", "THIRD_PARTY.md", "LICENSE"):
        source = root / name
        if source.exists():
            shutil.copy2(source, deployment / name)
    print("Application: " + str(executable))
