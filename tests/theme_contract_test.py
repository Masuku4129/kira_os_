#!/usr/bin/env python3
"""Guard the production theme against accidental drift from the Spatial Minimal contract."""
from pathlib import Path
import re

source = Path(__file__).resolve().parents[1].joinpath("theme.h").read_text(encoding="utf-8")
expected = {
    "TH_BG0": "0x00070D19u",
    "TH_PANEL1": "0x000C1629u",
    "TH_TILE": "0x0013213Au",
    "TH_BORDER": "0x00263653u",
    "TH_ACCENT_PRIMARY": "0x009B6BFFu",
    "TH_ACCENT_SECONDARY": "0x0042A5FFu",
    "TH_STATE_SUCCESS": "0x0035C995u",
    "TH_STATE_WARNING": "0x00F2B84Bu",
    "TH_STATE_DANGER": "0x00F07178u",
    "TH_TEXT": "0x00F1F3FAu",
    "TH_TEXT_DIM": "0x00AAB5CCu",
    "TH_RADIUS_CONTROL": "12",
    "TH_RADIUS_SURFACE": "20",
}
for name, value in expected.items():
    pattern = rf"^#define\s+{name}\s+{re.escape(value)}\s*$"
    if not re.search(pattern, source, re.MULTILINE):
        raise SystemExit(f"FAIL: {name} must be {value}")
for name, target in {
    "TH_CYAN": "TH_ACCENT_PRIMARY",
    "TH_PURPLE": "TH_ACCENT_PRIMARY",
    "TH_BLUE": "TH_ACCENT_SECONDARY",
    "TH_GREEN": "TH_STATE_SUCCESS",
    "TH_AMBER": "TH_STATE_WARNING",
    "TH_RED": "TH_STATE_DANGER",
}.items():
    if not re.search(rf"^#define\s+{name}\s+{target}\s*$", source, re.MULTILINE):
        raise SystemExit(f"FAIL: legacy token {name} must alias {target}")
print("theme: Spatial Minimal semantic palette and geometry tokens passed")
