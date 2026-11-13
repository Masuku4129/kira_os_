#!/usr/bin/env python3
"""Guard the Spatial Minimal partial-redraw contract against full-screen hover damage."""
from pathlib import Path
import sys

source = (Path(__file__).resolve().parents[1] / "wm.c").read_text(encoding="utf-8")


def body_after(signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:pos]
    raise AssertionError(f"unterminated function: {signature}")


dock = body_after("static void damage_side(void)")
nav = body_after("static void damage_spatial_tab(int i)")
hover = body_after("static void update_hover(void)")

if "spatial_dock_y() - 40" not in dock:
    raise AssertionError("Spatial Minimal Dock damage must be limited to its bottom band")
if "damage(0, WM_TOP_H, scr->w, scr->h)" in dock:
    raise AssertionError("Dock damage regressed to repainting the whole desktop")
if "WM_SPATIAL_RAIL_W" not in nav or "gfx_text_width(tabs[i].label)" not in nav:
    raise AssertionError("navigation damage must cover the rail icon and only its label tooltip")
if "damage_spatial_tab(old_tab)" not in hover or "damage_spatial_tab(th)" not in hover:
    raise AssertionError("navigation hover must repaint only the previous and current rail items")

print("Spatial Minimal damage bounds: Dock bottom band and per-item navigation repaint passed")
