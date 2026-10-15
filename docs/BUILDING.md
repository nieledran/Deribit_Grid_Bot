# Building and packaging

Use Windows x64, Visual Studio 2022 with C++/Windows SDK/CMake tools, Python 3,
and Qt 6.8.3 MSVC x64 with WebSockets. `aqtinstall` is one optional way to install
Qt; an existing Qt installation works too. `aqtinstall.log` is disposable installer output.

```powershell
python scripts/dependencies.py
python scripts/build.py --qt C:/Qt/6.8.3/msvc2022_64
```

This builds Release, runs CTest, backs up existing deployment/profile data,
and deploys to `dist/`. Close running bot windows before deploying. Paths are
resolved from the repository, so the current terminal directory does not matter.
You can use `QT_ROOT` or the repository-local `.deps/Qt/6.8.3/msvc2022_64`
instead of `--qt`. Visual Studio is discovered with `vswhere`; CMake on PATH also works.
Release builds omit Boost source-location metadata to keep local checkout paths out of
the EXE; checked arithmetic remains enabled. Development builds retain that metadata.

- `--skip-deploy`: build/test without updating the local installation.
- `--deploy-dir build/public-runtime`: deploy separately while the local bot is running.
- `--core-only`: build/test the engine without Qt.
- `--asan --skip-deploy`: build/test with AddressSanitizer.
- `--benchmarks --skip-deploy`: also build developer performance probes.

## Portable release

To prepare a release separately from your working installation:

```powershell
python scripts/build.py --qt C:/Qt/6.8.3/msvc2022_64 --deploy-dir build/public-runtime
python scripts/package.py --qt C:/Qt/6.8.3/msvc2022_64 --dist build/public-runtime
```

The packager creates `releases/DeribitGrid-0.1.0-windows-x64.zip` and its SHA-256
checksum. It takes only runtime files, current documentation, and licenses;
profiles, databases, logs, debug symbols, and local backups are excluded.
It checks the EXE for embedded local build/user paths and verifies required Qt plugins.
The first run downloads the official Qt 6.8.3 source archives and checks their
published SHA-256 hashes; these sources and their license texts are bundled in the ZIP.
Packaging currently requires the pinned Qt 6.8.3 runtime.

Extract the ZIP into a different writable folder and run `start.cmd` before uploading
it to GitHub Releases. Git keeps source only; `dist/`, `releases/`, `.deps/`, and
all private profiles stay ignored. Never upload the working `dist/` folder directly.

`CMakeLists.txt` is the build definition and must stay in the repository.
Visual Studio can open this folder directly; CMake generates its own solution under
`build/app/`. The obsolete root solution referenced a missing project and was removed.
