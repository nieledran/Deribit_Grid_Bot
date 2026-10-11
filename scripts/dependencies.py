"""Install the pinned header-only arithmetic dependency from the official Boost archive."""
from pathlib import Path
import hashlib
import urllib.request
import zipfile

root = Path(__file__).resolve().parents[1]
archive = root / ".deps/boost_1_89_0.zip"
archive.parent.mkdir(exist_ok=True)
if not archive.exists():
    urllib.request.urlretrieve("https://archives.boost.io/release/1.89.0/source/boost_1_89_0.zip", archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != "77bee48e32cabab96a3fd2589ec3ab9a17798d330220fdd8bde6ff5611b4ccde":
    raise SystemExit("Boost archive checksum mismatch")
with zipfile.ZipFile(archive) as package:
    for name in package.namelist():
        if name.startswith("boost_1_89_0/boost/") or name == "boost_1_89_0/LICENSE_1_0.txt":
            package.extract(name, root / ".deps")
print("Boost 1.89.0 headers installed")
