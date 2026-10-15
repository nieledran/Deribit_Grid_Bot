# Third-party components

Deribit Grid's own code is MIT licensed; see `LICENSE`.

## Qt 6.8.3

The Windows release dynamically links unmodified Qt Core, GUI, Widgets, Network,
WebSockets and SQL libraries, including the SQLite driver and deployment plugins.
Qt is copyright The Qt Company Ltd. and other contributors. This distribution
uses Qt under LGPL v3; Qt and its bundled components retain their own licenses.
Users may replace compatible Qt DLLs, and reverse engineering for debugging
modifications to the LGPL libraries is permitted.

The portable ZIP includes Qt license texts in `licenses/Qt-6.8.3/`, the supplied
software bills of materials in `sbom/`, and the complete corresponding QtBase and
QtWebSockets source archives in `sources/`. These archives also contain upstream
third-party notices and build instructions. Qt source is available separately at
[the official 6.8.3 archive](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/).
See [Qt licensing](https://doc.qt.io/qt-6.8/licensing.html).

## Boost 1.89.0

Boost.Multiprecision supplies checked integer arithmetic as a header-only dependency.
The Boost Software License is retained in `licenses/Boost-1.89.0.txt` and the ZIP.
No Boost DLLs are required.

## Microsoft runtime

The Windows ZIP includes Microsoft Visual C++ runtime and Direct3D compiler DLLs
supplied through the Visual Studio/Qt deployment tools. These remain Microsoft's
components under their respective redistribution terms, rather than the project's MIT license.

Python and aqtinstall are build/setup tools and are not required by the executable.
The Python deribit-wrapper is not used or bundled.
