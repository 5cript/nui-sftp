#!/usr/bin/env python3
"""
Merge the C++ SPDX manifest and the npm-checker output into a single
runtime-friendly licenses.json, optionally producing a release tarball
and/or a C++ header that embeds the bundle as a string literal for
compile-time inclusion into the WASM frontend.

Usage:
    python scripts/build_licenses_bundle.py --out path/to/licenses.json
    python scripts/build_licenses_bundle.py --out ... --tarball path/to/tarball.tar.gz
    python scripts/build_licenses_bundle.py --out ... --header path/to/licenses_data.hpp
    python scripts/build_licenses_bundle.py --out ... --npm-licenses path/to/npm-licenses.json

The runtime JSON has the shape expected by the Licenses frontend page:

    {
      "cpp": [
        { "name", "version", "license", "homepage",
          "downloadLocation", "copyright", "role", "text" }, ...
      ],
      "npm": [
        { "name", "version", "license", "publisher", "url", "text" }, ...
      ]
    }

`text` is the full license body inlined as a string. The tarball, if
requested, contains the SPDX file, every license-text file, and the
generated licenses.json.

The header, if requested, exposes the bundle as `LicenseData::json`
(a `std::string_view`) so the frontend can read it directly without a
runtime file lookup.
"""
from __future__ import annotations

import argparse
import json
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPDX_FILE = REPO / "licenses" / "third_party.spdx.json"
TEXTS_DIR = REPO / "licenses" / "texts"
DEFAULT_NPM_FILE = REPO / "licenses" / "npm-licenses.json"

# Raw-string delimiter for the embedded C++ header. Must not appear as the
# closing sequence `)<DELIM>"` anywhere inside the JSON (extremely unlikely
# in practice, but checked at generation time).
HEADER_DELIM = "NUI_LIC_EMBED"


def build_cpp_section() -> list[dict]:
    doc = json.loads(SPDX_FILE.read_text(encoding="utf-8"))
    out: list[dict] = []
    for element in doc.get("@graph", []):
        if element.get("type") != "software_Package":
            continue
        name = element.get("name", "")
        text_ref = element.get("extension_licenseTextFile", "")
        text = ""
        if text_ref:
            text_path = (SPDX_FILE.parent / text_ref).resolve()
            if text_path.exists():
                text = text_path.read_text(encoding="utf-8", errors="replace")
        out.append(
            {
                "name": name,
                "version": element.get("software_packageVersion", ""),
                "license": element.get("simplelicensing_declaredLicense", ""),
                "homepage": element.get("software_homePage", ""),
                "downloadLocation": element.get("software_downloadLocation", ""),
                "copyright": element.get("software_copyrightText", ""),
                "role": element.get("extension_role", ""),
                "text": text,
            }
        )
    out.sort(key=lambda p: p["name"].lower())
    return out


def build_npm_section(npm_file: Path, host_root: Path | None) -> list[dict]:
    if not npm_file.exists():
        return []
    raw = json.loads(npm_file.read_text(encoding="utf-8"))
    # license-checker reports the host project itself as if it were a dep
    # (with license=UNKNOWN since our package.json has no license field).
    # Drop any entry whose path equals the directory license-checker was
    # invoked against.
    host_resolved = host_root.resolve() if host_root else None
    out: list[dict] = []
    for key, info in raw.items():
        pkg_path = info.get("path")
        if host_resolved and pkg_path and Path(pkg_path).resolve() == host_resolved:
            continue
        # license-checker keys look like "name@version".
        if "@" in key.lstrip("@"):
            head, _, version = key.rpartition("@")
            name = head if head else key
        else:
            name, version = key, ""
        text = ""
        license_file = info.get("licenseFile")
        if license_file:
            p = Path(license_file)
            if p.exists() and p.is_file():
                try:
                    text = p.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    text = ""
        if not text:
            pkg_path = info.get("path")
            if pkg_path:
                base = Path(pkg_path)
                for candidate in ("LICENSE", "LICENSE.md", "LICENSE.txt",
                                  "license", "license.md", "LICENCE",
                                  "COPYING", "COPYING.txt"):
                    p = base / candidate
                    if p.exists() and p.is_file():
                        try:
                            text = p.read_text(encoding="utf-8", errors="replace")
                            break
                        except OSError:
                            continue
        out.append(
            {
                "name": name,
                "version": version,
                "license": info.get("licenses", ""),
                "publisher": info.get("publisher", ""),
                "url": info.get("repository", "") or info.get("url", ""),
                "text": text,
            }
        )
    out.sort(key=lambda p: p["name"].lower())
    return out


def write_tarball(tarball_path: Path, runtime_json: Path, npm_file: Path) -> None:
    tarball_path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball_path, "w:gz") as tar:
        tar.add(SPDX_FILE, arcname="licenses/third_party.spdx.json")
        tar.add(TEXTS_DIR, arcname="licenses/texts")
        tar.add(runtime_json, arcname="licenses/licenses.json")
        if npm_file.exists():
            tar.add(npm_file, arcname="licenses/npm-licenses.json")


def write_header(header_path: Path, json_text: str) -> None:
    closing = f"){HEADER_DELIM}\""
    if closing in json_text:
        raise SystemExit(
            f"Generated JSON contains the raw-string delimiter {closing!r}. "
            f"Pick a different HEADER_DELIM in build_licenses_bundle.py."
        )
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text(
        "// Generated by scripts/build_licenses_bundle.py — do not edit.\n"
        "#pragma once\n"
        "\n"
        "#include <string_view>\n"
        "\n"
        "namespace LicenseData\n"
        "{\n"
        f"    inline constexpr std::string_view json = R\"{HEADER_DELIM}(\n"
        f"{json_text}\n"
        f"){HEADER_DELIM}\";\n"
        "}\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path,
                        help="Path to write the merged licenses.json")
    parser.add_argument("--tarball", type=Path, default=None,
                        help="Optional path to write a release tarball")
    parser.add_argument("--header", type=Path, default=None,
                        help="Optional path to write a C++ header embedding the bundle")
    parser.add_argument("--npm-licenses", type=Path, default=DEFAULT_NPM_FILE,
                        dest="npm_licenses",
                        help="Path to the license-checker JSON output (defaults to "
                             f"{DEFAULT_NPM_FILE.relative_to(REPO)})")
    parser.add_argument("--npm-host-root", type=Path, default=None,
                        dest="npm_host_root",
                        help="Directory license-checker was invoked against. The "
                             "package living there is the host project itself "
                             "and is excluded from the npm section.")
    args = parser.parse_args()

    if not SPDX_FILE.exists():
        print(f"error: {SPDX_FILE} not found", file=sys.stderr)
        return 2

    bundle = {
        "cpp": build_cpp_section(),
        "npm": build_npm_section(args.npm_licenses, args.npm_host_root),
    }
    json_text = json.dumps(bundle, indent=2, ensure_ascii=False)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json_text, encoding="utf-8")
    print(f"wrote {args.out} ({len(bundle['cpp'])} cpp, {len(bundle['npm'])} npm)")

    if args.tarball:
        write_tarball(args.tarball, args.out, args.npm_licenses)
        print(f"wrote {args.tarball}")

    if args.header:
        write_header(args.header, json_text)
        print(f"wrote {args.header}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
