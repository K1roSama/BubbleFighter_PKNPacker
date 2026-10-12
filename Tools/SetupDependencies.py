"""Project-local downloads from the official Qt and upstream repositories."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
import xml.etree.ElementTree as ET
import zipfile
import requests

REPOSITORY = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111_msvc2022_64/"
ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = ROOT / "Artifacts/Dependencies"


def fetch(url):
    response = requests.get(url, timeout=60)
    response.raise_for_status()
    return response.content


def download(url, name, sha1=None):
    target = DEPENDENCIES / "Downloads" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and (not sha1 or hashlib.sha1(target.read_bytes()).hexdigest() == sha1):
        return target
    part = target.with_suffix(target.suffix + ".part")
    response = requests.get(url, stream=True, timeout=(30, 90))
    response.raise_for_status()
    size = int(response.headers.get("Content-Length", 0))
    amount, last = 0, time.monotonic()
    print("Download " + name, flush=True)
    with part.open("wb") as file:
        for chunk in response.iter_content(1024 * 1024):
            file.write(chunk)
            amount += len(chunk)
            if time.monotonic() - last > 10:
                print("  %d / %d bytes" % (amount, size), flush=True)
                last = time.monotonic()
    if sha1 and hashlib.sha1(part.read_bytes()).hexdigest() != sha1:
        raise RuntimeError("Official SHA-1 mismatch for " + name)
    part.replace(target)
    return target


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inspect", action="store_true")
    args = parser.parse_args()
    data = fetch(REPOSITORY + "Updates.xml")
    xml = ET.fromstring(data)
    package = next(p for p in xml.findall("PackageUpdate") if p.findtext("Name") == "qt.qt6.6111.win64_msvc2022_64")
    fields = {child.tag: child.text for child in package}
    print(json.dumps(fields, ensure_ascii=True), flush=True)
    if args.inspect:
        print(fetch(REPOSITORY + fields["Name"] + "/").decode("utf-8"), flush=True)
        return
    import py7zr
    archives = [s.strip() for s in fields["DownloadableArchives"].split(",")]
    # Core/Gui/Widgets/Test and Chinese translations for standard Qt dialogs.
    archives = [a for a in archives if a.startswith(("qtbase", "qttranslations"))]
    if not archives:
        raise RuntimeError("Official metadata did not advertise qtbase")
    records = []
    for archive in archives:
        filename = fields["Version"] + archive
        url = REPOSITORY + fields["Name"] + "/" + filename
        expected = fetch(url + ".sha1").decode("ascii").strip().split()[0]
        path = download(url, filename, expected)
        print("Extract " + filename, flush=True)
        with py7zr.SevenZipFile(path, mode="r") as package_archive:
            names = package_archive.getnames()
            if any(Path(n).is_absolute() or ".." in Path(n).parts for n in names):
                raise RuntimeError("Unsafe archive member")
            package_archive.extractall(path=DEPENDENCIES / "Qt")
        records.append({"Url": url, "Sha1": expected, "Sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    qt_msbuild_url = "https://download.qt.io/official_releases/vsaddin/3.4.1/qt-vsaddin-msbuild-3.4.1.zip"
    path = download(qt_msbuild_url, "qt-vsaddin-msbuild-3.4.1.zip")
    with zipfile.ZipFile(path) as archive:
        if any(Path(n).is_absolute() or ".." in Path(n).parts for n in archive.namelist()):
            raise RuntimeError("Unsafe archive member")
        archive.extractall(DEPENDENCIES / "QtMsBuild")
    records.append({"Url": qt_msbuild_url, "Sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    (DEPENDENCIES / "Downloads/DependencyReceipt.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    print("Project-local Qt dependencies ready", flush=True)


if __name__ == "__main__":
    main()
