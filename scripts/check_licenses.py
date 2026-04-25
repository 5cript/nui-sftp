#!/usr/bin/env python3
"""
Check that every C++ dependency declared in CMake has a matching entry in
licenses/third_party.spdx.json (and vice versa for non-system entries), and
that every SPDX entry points at a non-empty license text file that isn't
still a PLACEHOLDER.

Run from the repo root:
    python scripts/check_licenses.py

Exits non-zero on any drift.

Names are matched case-insensitively after stripping non-alphanumeric
characters: "OpenSSL" / "openssl" / "Open_SSL" all collapse to "openssl".
Use ALIASES for variants that don't normalise equal (e.g. "promise" vs
"promise-cpp"), and SKIP for build-tooling deps that don't ship in the
release binary.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPDX_FILE = REPO / "licenses" / "third_party.spdx.json"

# CMake files to scan. Globs are evaluated relative to REPO.
SCAN_GLOBS = [
    "CMakeLists.txt",
    "_cmake/dependencies/*.cmake",
    "_cmake/offline_build.cmake",
    "_cmake/dependencies.cmake",
    "dependencies/Nui/cmake/dependencies/*.cmake",
    "dependencies/roar/cmake/dependencies/*.cmake",
]

# Build-tooling and test-only deps that don't ship in the release binary.
# Compared in normalised form (lowercase, alphanumeric only).
SKIP_NORM = {
    "pkgconfig",
    "doxygen",
    "git",
    "python3",
    "gtest",
    "googletest",
    "emscripten",        # build toolchain, not linked
    "binaryenrelease",   # build toolchain, not linked
    # Boost sub-libraries — covered by a single 'boost' SPDX entry.
    "boostdescribe",
    "boostmp11",
    "boostpreprocessor",
}

# CMake declaration name -> canonical SPDX package name (raw, not normalised).
# Use this when the variant names don't collapse equal under normalisation.
ALIASES = {
    "webview_raw": "webview",
    "webview-binary-nui": "webview",
    "promise": "promise-cpp",
    "traits-library": "traits",
    "CryptoPP": "Crypto++",
}

# SPDX entries that legitimately have no CMake declaration (e.g. system
# packages pulled transitively from pacman, or ones detected via
# pkg_search_module which the regex doesn't catch).
NO_CMAKE_DECLARATION_NORM = {
    "nuisftp",       # the project itself
    "boost",         # transitively via roar / many headers
    "webkitgtk",     # detected via pkg_search_module, not find_package
    "ui5sapicons",   # vendored inside dependencies/5cript-nui-components
}

FIND_PACKAGE_RE = re.compile(r"\bfind_package\s*\(\s*([A-Za-z0-9_\-]+)")
FETCH_CONTENT_RE = re.compile(
    r"\bFetchContent_Declare\s*\(\s*([A-Za-z0-9_\-]+)", re.MULTILINE
)
ADD_SUBDIR_DEPS_RE = re.compile(
    r'\badd_subdirectory\s*\(\s*"?\$\{[A-Z_]+\}/dependencies/([A-Za-z0-9_\-]+)'
)
NUI_FETCH_RE = re.compile(
    r"\bnui_fetch_dependency\s*\([^)]*?LIBRARY_NAME\s+([A-Za-z0-9_\-]+)",
    re.DOTALL,
)


def normalise(name: str) -> str:
    return re.sub(r"[^a-z0-9]", "", name.lower())


def extract_from_cmake() -> dict[str, list[str]]:
    """Return {normalised_name: [source_locations...]} from CMake source.

    Aliases are resolved before normalisation; SKIP entries are dropped.
    """
    found: dict[str, list[str]] = {}

    def record(raw: str, where: str) -> None:
        canonical = ALIASES.get(raw, raw)
        norm = normalise(canonical)
        if norm in SKIP_NORM:
            return
        found.setdefault(norm, []).append(where)

    for glob in SCAN_GLOBS:
        for path in sorted(REPO.glob(glob)):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            rel = path.relative_to(REPO).as_posix()
            for pattern in (
                FIND_PACKAGE_RE,
                FETCH_CONTENT_RE,
                ADD_SUBDIR_DEPS_RE,
                NUI_FETCH_RE,
            ):
                for m in pattern.finditer(text):
                    line = text[: m.start()].count("\n") + 1
                    record(m.group(1), f"{rel}:{line}")
    return found


def load_spdx() -> dict[str, dict]:
    """Return {normalised_name: full_element} for every software_Package."""
    doc = json.loads(SPDX_FILE.read_text(encoding="utf-8"))
    packages: dict[str, dict] = {}
    for element in doc.get("@graph", []):
        if element.get("type") != "software_Package":
            continue
        name = element.get("name")
        if not name:
            continue
        packages[normalise(name)] = element
    return packages


def main() -> int:
    if not SPDX_FILE.exists():
        print(f"error: {SPDX_FILE.relative_to(REPO)} not found", file=sys.stderr)
        return 2

    spdx_packages = load_spdx()
    spdx_norm = set(spdx_packages.keys())

    cmake_found = extract_from_cmake()
    cmake_norm = set(cmake_found.keys())

    errors: list[str] = []

    missing_in_spdx = sorted(cmake_norm - spdx_norm)
    for name in missing_in_spdx:
        locs = ", ".join(cmake_found[name])
        errors.append(
            f"  - {name!r} declared in CMake at [{locs}] but missing from "
            f"licenses/third_party.spdx.json. Add a software_Package entry."
        )

    stale_in_spdx = sorted((spdx_norm - cmake_norm) - NO_CMAKE_DECLARATION_NORM)
    for name in stale_in_spdx:
        display = spdx_packages[name].get("name", name)
        errors.append(
            f"  - {display!r} listed in SPDX but no CMake declaration found. "
            f"Either remove the SPDX entry, alias the CMake name in "
            f"scripts/check_licenses.py, or add it to NO_CMAKE_DECLARATION_NORM."
        )

    for name, element in sorted(spdx_packages.items()):
        display = element.get("name", name)
        text_ref = element.get("extension_licenseTextFile")
        if not text_ref:
            errors.append(f"  - {display!r} has no extension_licenseTextFile field.")
            continue
        text_path = (SPDX_FILE.parent / text_ref).resolve()
        if not text_path.exists():
            errors.append(
                f"  - {display!r} references missing license text file "
                f"{text_ref!r} (resolved: {text_path})."
            )
            continue
        body = text_path.read_text(encoding="utf-8", errors="replace").strip()
        if not body:
            errors.append(f"  - {display!r}: license text file {text_ref!r} is empty.")
        elif body.upper().startswith("PLACEHOLDER"):
            errors.append(
                f"  - {display!r}: license text file {text_ref!r} is still a "
                f"PLACEHOLDER. Paste the real license body."
            )

    if errors:
        print("License manifest is out of sync:", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        print(
            f"\n{len(errors)} issue(s). See licenses/README.md for guidance.",
            file=sys.stderr,
        )
        return 1

    print(f"OK: {len(spdx_norm)} packages, no drift.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
