#!/usr/bin/env python3
"""Reject legacy Orca accent colors in InlongSlicer user-interface sources."""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCAN_ROOTS = (
    REPO_ROOT / "src" / "slic3r" / "GUI",
    REPO_ROOT / "resources" / "web",
    REPO_ROOT / "resources" / "images",
)
SCAN_FILES = (
    REPO_ROOT / "scripts" / "flatpak" / "io.github.JaredLin1217.InlongSlicer.metainfo.xml",
)
TEXT_SUFFIXES = {".cpp", ".h", ".hpp", ".css", ".html", ".js", ".svg"}

LEGACY_PATTERNS = {
    "legacy Orca hex accent": re.compile(
        r"#(?:009688|009789|009687|26a69a|00897b|00695c|00675b|008172|244945|5eead4|8de5d6)\b",
        re.IGNORECASE,
    ),
    "legacy Orca integer RGB": re.compile(
        r"(?:\b0\s*,\s*150\s*,\s*136\b|"
        r"\b0\s*,\s*137\s*,\s*123\b|"
        r"\b38\s*,\s*166\s*,\s*154\b|"
        r"\b0\s*,\s*129\s*,\s*114\b|"
        r"\b0\s*,\s*103\s*,\s*91\b)"
    ),
    "legacy Orca decimal RGB": re.compile(
        r"(?:\b0(?:\.0+)?f?\s*,\s*0\.59f?\s*,\s*0\.53f?\b|"
        r"\b0(?:\.0+)?f?\s*,\s*0\.588f?\s*,\s*0\.533f?\b)"
    ),
    "legacy Orca normalized RGB": re.compile(
        r"(?:\b0(?:\.0+)?f?\s*(?:/\s*255(?:\.0)?f?)?\s*,\s*"
        r"(?:150|137|129|103)(?:\.0)?f?\s*/\s*255(?:\.0)?f?\s*,\s*"
        r"(?:136|123|114|91)(?:\.0)?f?\s*/\s*255(?:\.0)?f?\b|"
        r"\b38(?:\.0)?f?\s*/\s*255(?:\.0)?f?\s*,\s*"
        r"166(?:\.0)?f?\s*/\s*255(?:\.0)?f?\s*,\s*"
        r"154(?:\.0)?f?\s*/\s*255(?:\.0)?f?\b)"
    ),
}

CANONICAL_ANCHORS = {
    REPO_ROOT / "src" / "libslic3r" / "Color.hpp": (
        re.compile(
            r"ColorRGB\s+INLONG\(\).*?214(?:\.0)?f?\s*/\s*255(?:\.0)?f?.*?"
            r"108(?:\.0)?f?\s*/\s*255(?:\.0)?f?.*?71(?:\.0)?f?\s*/\s*255(?:\.0)?f?",
            re.DOTALL,
        ),
        re.compile(
            r"ColorRGBA\s+INLONG\(\).*?214(?:\.0)?f?\s*/\s*255(?:\.0)?f?.*?"
            r"108(?:\.0)?f?\s*/\s*255(?:\.0)?f?.*?71(?:\.0)?f?\s*/\s*255(?:\.0)?f?",
            re.DOTALL,
        ),
    ),
    REPO_ROOT / "resources" / "web" / "include" / "global.css": (
        re.compile(r"--main-color\s*:\s*#D66C47", re.IGNORECASE),
    ),
    REPO_ROOT / "src" / "slic3r" / "GUI" / "Widgets" / "StateColor.cpp": (
        re.compile(r'\{"#D66C47"\s*,\s*"#A64E33"\}', re.IGNORECASE),
        re.compile(r'\{"#E18263"\s*,\s*"#B9573A"\}', re.IGNORECASE),
    ),
    REPO_ROOT / "src" / "slic3r" / "GUI" / "Widgets" / "WebViewHostDialog.cpp": (
        re.compile(r'wxColour\("#D66C47"\)', re.IGNORECASE),
    ),
    REPO_ROOT / "scripts" / "flatpak" / "io.github.JaredLin1217.InlongSlicer.metainfo.xml": (
        re.compile(
            r'<color\s+type="primary"\s+scheme_preference="light">#D66C47</color>',
            re.IGNORECASE,
        ),
        re.compile(
            r'<color\s+type="primary"\s+scheme_preference="dark">#A64E33</color>',
            re.IGNORECASE,
        ),
    ),
}


def iter_ui_files():
    for root in SCAN_ROOTS:
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES:
                yield path
    yield from SCAN_FILES


def main() -> int:
    failures: list[str] = []

    for path in iter_ui_files():
        try:
            text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            continue
        relative = path.relative_to(REPO_ROOT)
        for line_number, line in enumerate(text.splitlines(), start=1):
            for label, pattern in LEGACY_PATTERNS.items():
                if pattern.search(line):
                    failures.append(f"{relative}:{line_number}: {label}: {line.strip()}")

    for path, patterns in CANONICAL_ANCHORS.items():
        if not path.is_file():
            failures.append(f"{path.relative_to(REPO_ROOT)}: missing canonical brand anchor")
            continue
        text = path.read_text(encoding="utf-8-sig")
        for pattern in patterns:
            if not pattern.search(text):
                failures.append(
                    f"{path.relative_to(REPO_ROOT)}: canonical Inlong brand anchor not found: {pattern.pattern}"
                )

    if failures:
        print("Inlong brand color validation failed:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("Inlong brand color validation passed: no legacy Orca accents found in UI sources.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
