# Third-party components

DeribitGrid links dynamically to Qt 6 (Core, GUI, Widgets, Network, WebSockets, SQL). The SQLite driver is supplied by Qt. Qt libraries retain their original licences and notices; this project does not modify them.

The local build uses Qt 6.8.3, downloaded from official Qt distribution mirrors with aqtinstall. aqtinstall and Python are build tools and are not used by the trading executable.

Before redistributing a build, provide the required Qt licence texts, notices and corresponding source/access to source for the exact Qt version, or use an appropriate commercial Qt licence. See [Qt licensing](https://www.qt.io/licensing/open-source-lgpl-obligations) and [Qt 6.8 sources](https://download.qt.io/archive/qt/6.8/6.8.3/single/).

The AntonioVentilii Python deribit-wrapper is not bundled, imported or linked.

Boost.Multiprecision 1.89.0 is used as a header-only dependency for checked fixed-width integer arithmetic. Install the pinned headers with `python scripts/dependencies.py`. The Boost Software License is supplied in `licenses/Boost-1.89.0.txt` and copied into the distribution. No Boost DLLs are required.

State JSON uses the Qt Core JSON implementation already supplied with the application.
