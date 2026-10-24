"""
THE HYBRID CORE — faceplate layout (single source of truth).

Every live control's position on the faceplate art, in ORIGINAL IMAGE pixels
(skin_original.jpg is 1438 x 2576). This script:
  1. cuts the painted knobs, meters, screens and readouts out of the art
     (fills them with the surrounding panel colour) -> assets/skin.jpg
  2. writes skin_layout.hpp so the C++ app places its real controls on the
     exact same spots.
Run:  python3 studio/skin_layout.py
"""
from PIL import Image, ImageDraw, ImageFilter
import os, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "assets", "skin_original.jpg")
DST = os.path.join(HERE, "assets", "skin.jpg")
HPP = os.path.join(HERE, "skin_layout.hpp")

# ---- knobs: (name, cx, cy, radius_of_cutout) --------------------------------
KNOBS = [
    ("dsThreshold", 303, 1623, 46), ("dsBleed", 476, 1623, 46), ("dsDecay", 597, 1623, 46),
    ("rsSensitivity", 838, 1623, 46), ("rsFrequency", 958, 1623, 46), ("rsBandwidth", 1078, 1623, 46),
    ("rsReduction", 1198, 1623, 46), ("rsCompensation", 1316, 1623, 46),
    ("trAttack", 117, 2052, 54), ("trSustain", 265, 2052, 54), ("trGain", 462, 2052, 60), ("trMix", 600, 2052, 54),
    ("msCeiling", 1228, 1848, 42), ("msThreshold", 1330, 1848, 42),
    ("msRelease", 1228, 2072, 45), ("msWidth", 1330, 2072, 45),
]
# ---- rectangles: (name, x0, y0, x1, y1) -------------------------------------
RECTS = [
    # transport
    ("timeDisplay", 420, 578, 756, 627), ("tempoDisplay", 768, 578, 874, 627), ("levelMeter", 893, 579, 1150, 627),
    # arrange
    ("ruler", 322, 655, 1362, 686), ("lanes", 322, 688, 1350, 1245), ("names", 104, 688, 238, 1245),
    ("rowsAll", 56, 688, 1354, 1245),
    # header selectors (text boxes)
    ("dsHeaderBox", 509, 1324, 608, 1352), ("rsHeaderBox", 1162, 1324, 1314, 1352), ("msHeaderBox", 1164, 1750, 1310, 1774),
    # screens
    ("dsScreen", 82, 1375, 650, 1571), ("rsScreen", 794, 1376, 1358, 1568),
    ("trAttackScreen", 76, 1796, 354, 1978), ("trSustainScreen", 366, 1796, 651, 1978),
    # destroyer resolution selector box, bypass toggle
    ("dsResBox", 123, 1614, 186, 1646), ("dsToggle", 375, 1612, 405, 1648),
    # transient punch/speed switch + LED
    ("trSwitch", 348, 2036, 378, 2068), ("trLed", 596, 1746, 646, 1770),
    # mastering meters + readouts
    ("msMeters", 792, 1824, 1010, 2100),
    ("msIntegrated", 1022, 1832, 1146, 1868), ("msShortTerm", 1022, 1905, 1146, 1941),
    ("msPsr", 1022, 1984, 1146, 2020), ("msPlr", 1022, 2057, 1146, 2093),
    ("msPlrLabel", 1052, 2031, 1118, 2051), ("msFooterLabel", 1052, 2101, 1118, 2122),
    ("msModeBox", 1192, 1935, 1268, 1961), ("msTargetBox", 1296, 1935, 1368, 1961), ("msTargetLabel", 1286, 1986, 1376, 2007),
]
# ---- clickable regions --------------------------------------------------------------------------
# Every BUTTON here is cut out of the art and drawn live by the app (real hover / press / lit states);
# the plug-in titles (dsTitle, rsTitle, trTitle, msTitle) keep their printed lettering and are click
# areas for bypass, with the bypass state drawn live over them.
HOTSPOTS = [
    # transport: six real buttons (MENU REWIND PLAY STOP RECORD LOOP) spread evenly over the painted row
    ("btnMenu", 76, 145, 126, 200),
    ("btnRewind", 132, 145, 182, 200),
    ("btnPlay", 187, 145, 237, 200),
    ("btnStop", 243, 145, 293, 200),
    ("btnRecord", 298, 145, 348, 200),
    ("btnLoop", 354, 145, 404, 200),
    ("btnAnalyze", 1163, 145, 1233, 200), ("btnSettings", 1243, 145, 1298, 200), ("btnView", 1306, 145, 1360, 200),
    ("btnTopLeft", 80, 96, 116, 126), ("btnTopRight", 1322, 96, 1358, 126),
    ("dsPrev", 467, 1322, 497, 1354), ("dsNext", 620, 1322, 652, 1354), ("dsTitle", 100, 1316, 440, 1356),
    ("rsPrev", 1120, 1322, 1148, 1354), ("rsNext", 1326, 1322, 1356, 1354), ("rsTitle", 798, 1316, 1060, 1356),
    ("dsResPrev", 82, 1613, 110, 1647), ("dsResNext", 198, 1613, 226, 1647),
    ("trTitle", 90, 1740, 340, 1780),
    ("msPrev", 1122, 1748, 1150, 1776), ("msNext", 1332, 1748, 1362, 1776), ("msTitle", 798, 1740, 1060, 1780),
]
# transport hotspots above are in "top crop" coordinates; shift into image space
HOTSPOTS = [(n, x0, y0 + 430, x1, y1 + 430) if n.startswith("btn") else (n, x0, y0, x1, y1)
            for (n, x0, y0, x1, y1) in HOTSPOTS]
TITLES = {"dsTitle", "rsTitle", "trTitle", "msTitle"}
# Whole strips of the art that held painted buttons (image coords), cleared before the live buttons are drawn.
BUTTON_STRIPS = [(72, 571, 407, 634), (1158, 571, 1365, 634)]
# track rows: centre y of row i = ROW0 + i * ROW_STEP
ROW0, ROW_STEP, ROW_H = 0.0, 0.0, 0.0   # computed from the 16:9 layout (see write_header)
ROW_M = (243, 271)   # x range of the painted M box
ROW_S = (276, 305)   # x range of the painted S box
# ---- 16:9 composition (canvas px). Controls keep ORIGINAL-image coordinates;
# the app draws each section with its own offset (scale is 1:1 for both).
CANVAS_W, CANVAS_H = 2876, 1618
TITLE_SRC = (0, 0, 1438, 430)        # crown + THE HYBRID CORE
BASE_SRC = (0, 2165, 1438, 2576)     # hood, vent, tagline
BAND_SCALE = 0.82
BAND_A_H = 354                       # title band height
CONSOLE_TOP = (452, 690)             # orig y: frame + transport + ruler
CONSOLE_ROWS = (690, 1245)           # orig y: track rows (stretched)
CONSOLE_BOT = (1245, 1272)           # orig y: bottom frame
MID_H = 910                          # console and panel block height
ROWS_Y1 = CONSOLE_ROWS[0] + (MID_H - (CONSOLE_TOP[1] - CONSOLE_TOP[0]) - (CONSOLE_BOT[1] - CONSOLE_BOT[0]))
PANELS_SRC = (0, 1286, 1438, 1286 + MID_H)
CONSOLE_DX, CONSOLE_DY = 0, BAND_A_H - CONSOLE_TOP[0]
PANELS_DX, PANELS_DY = 1438, BAND_A_H - PANELS_SRC[1]


def ring_median(im, cx, cy, r):
    px = im.load(); w, h = im.size; vals = []
    for rr in (r + 4, r + 7, r + 10):
        for a in range(0, 360, 6):
            import math
            x = int(cx + rr * math.cos(math.radians(a))); y = int(cy + rr * math.sin(math.radians(a)))
            if 0 <= x < w and 0 <= y < h: vals.append(px[x, y])
    return tuple(int(statistics.median(c[i] for c in vals)) for i in range(3))


def border_median(im, x0, y0, x1, y1):
    px = im.load(); w, h = im.size; vals = []
    for d in (2, 4):
        for x in range(x0, x1, 3):
            for y in (y0 - d, y1 + d):
                if 0 <= y < h: vals.append(px[x, y])
        for y in range(y0, y1, 3):
            for x in (x0 - d, x1 + d):
                if 0 <= x < w: vals.append(px[x, y])
    return tuple(int(statistics.median(c[i] for c in vals)) for i in range(3))


SCREEN_LIKE = {"rowsAll", "dsScreen", "rsScreen", "trAttackScreen", "trSustainScreen", "msMeters", "lanes", "ruler",
               "timeDisplay", "tempoDisplay", "levelMeter", "msIntegrated", "msShortTerm", "msPsr", "msPlr",
               "msModeBox", "msTargetBox", "dsHeaderBox", "rsHeaderBox", "msHeaderBox", "dsResBox"}


def build_skin():
    im = Image.open(SRC).convert("RGB")
    mask = Image.new("L", im.size, 0); md = ImageDraw.Draw(mask)
    fill = Image.new("RGB", im.size); fd = ImageDraw.Draw(fill)
    for name, cx, cy, r in KNOBS:
        col = ring_median(im, cx, cy, r)
        md.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
        fd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
    for name, x0, y0, x1, y1 in RECTS:
        col = (7, 11, 15) if name in SCREEN_LIKE else border_median(im, x0, y0, x1, y1)
        md.rectangle([x0, y0, x1, y1], fill=255)
        fd.rectangle([x0, y0, x1, y1], fill=col)
    for x0, y0, x1, y1 in BUTTON_STRIPS:
        col = border_median(im, x0, y0, x1, y1)
        md.rectangle([x0, y0, x1, y1], fill=255)
        fd.rectangle([x0, y0, x1, y1], fill=col)
    for name, x0, y0, x1, y1 in HOTSPOTS:
        if name in TITLES or name.startswith("btn") and not name.startswith("btnTop"):
            continue   # titles keep their lettering; transport buttons are covered by BUTTON_STRIPS
        x0, y0, x1, y1 = x0 - 2, y0 - 2, x1 + 2, y1 + 2
        col = border_median(im, x0, y0, x1, y1)
        md.rectangle([x0, y0, x1, y1], fill=255)
        fd.rectangle([x0, y0, x1, y1], fill=col)
    mask = mask.filter(ImageFilter.GaussianBlur(1.5))
    out = Image.composite(fill, im, mask)
    # HYBRID CORE 2 ELITE: only the middle band (console + the four panels) is used; the desk picture
    # replaces the title and base bands. skin.jpg row 0 = canvas row BAND_A_H (desk_layout.hpp kSkinY0).
    compose(out).crop((0, BAND_A_H, CANVAS_W, BAND_A_H + MID_H)).save(DST, quality=92, subsampling=0)
    return out


def feather_mask(w, h, edge):
    m = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(m)
    for i in range(edge):
        a = int(255 * (i + 1) / (edge + 1))
        d.rectangle([i, 0, i, h], fill=a)
        d.rectangle([w - 1 - i, 0, w - 1 - i, h], fill=a)
    return m


def band(src_img, box, height):
    """Scaled art centred on a backdrop made from the same art (blurred, darkened)."""
    piece = src_img.crop(box)
    pw, ph = int(piece.width * BAND_SCALE), int(piece.height * BAND_SCALE)
    piece = piece.resize((pw, ph), Image.LANCZOS)
    back = src_img.crop(box).resize((CANVAS_W, height), Image.LANCZOS).filter(ImageFilter.GaussianBlur(18))
    back = Image.eval(back, lambda v: int(v * 0.45))
    x = (CANVAS_W - pw) // 2
    y = (height - ph) // 2
    back.paste(piece, (x, y), feather_mask(pw, ph, 60))
    return back


def compose(cut):
    canvas = Image.new("RGB", (CANVAS_W, CANVAS_H), (6, 7, 9))
    canvas.paste(band(cut, TITLE_SRC, BAND_A_H), (0, 0))
    # console: top part 1:1, rows stretched, bottom frame 1:1
    top = cut.crop((0, CONSOLE_TOP[0], 1438, CONSOLE_TOP[1]))
    rows = cut.crop((0, CONSOLE_ROWS[0], 1438, CONSOLE_ROWS[1]))
    bot = cut.crop((0, CONSOLE_BOT[0], 1438, CONSOLE_BOT[1]))
    rows_h = MID_H - top.height - bot.height
    rows = rows.resize((1438, rows_h), Image.BICUBIC)
    canvas.paste(top, (0, BAND_A_H))
    canvas.paste(rows, (0, BAND_A_H + top.height))
    canvas.paste(bot, (0, BAND_A_H + top.height + rows_h))
    canvas.paste(cut.crop(PANELS_SRC), (1438, BAND_A_H))
    base_y = BAND_A_H + MID_H
    canvas.paste(band(cut, BASE_SRC, CANVAS_H - base_y), (0, base_y))
    return canvas


def write_header():
    L = ["// GENERATED by studio/skin_layout.py — do not edit by hand.",
         "// Positions of live controls on the faceplate art, in original image pixels.",
         "#pragma once", "namespace hc::studio::layout {",
         "struct Knob { float cx, cy, r; };", "struct Box { float x0, y0, x1, y1; };",
         "inline constexpr float kImageW = 1438.0f, kImageH = 2576.0f;"]
    for n, cx, cy, r in KNOBS:
        L.append(f"inline constexpr Knob {n}{{{cx}.0f, {cy}.0f, {r}.0f}};")
    for n, x0, y0, x1, y1 in RECTS + HOTSPOTS:
        L.append(f"inline constexpr Box {n}{{{x0}.0f, {y0}.0f, {x1}.0f, {y1}.0f}};")
    step = (ROWS_Y1 - CONSOLE_ROWS[0] - 8) / 13.0
    L.append(f"inline constexpr float kRowsY0 = {CONSOLE_ROWS[0]}.0f, kRowsY1 = {ROWS_Y1}.0f;")
    L.append(f"inline constexpr float kRow0 = {CONSOLE_ROWS[0] + 4 + step / 2:.3f}f, kRowStep = {step:.3f}f, kRowH = {step - 4:.3f}f;")
    L.append(f"inline constexpr float kCanvasW = {CANVAS_W}.0f, kCanvasH = {CANVAS_H}.0f;")
    L.append(f"inline constexpr float kConsoleDx = {CONSOLE_DX}.0f, kConsoleDy = {CONSOLE_DY}.0f;")
    L.append(f"inline constexpr float kMidY0 = {BAND_A_H}.0f, kMidY1 = {BAND_A_H + MID_H}.0f;   // canvas band holding the console (left) and the 2x2 rack (right)")
    L.append(f"inline constexpr float kPanelsDx = {PANELS_DX}.0f, kPanelsDy = {PANELS_DY}.0f;")
    L.append(f"inline constexpr Box rowM{{{ROW_M[0]}.0f, 0.0f, {ROW_M[1]}.0f, 0.0f}}, rowS{{{ROW_S[0]}.0f, 0.0f, {ROW_S[1]}.0f, 0.0f}};")
    L.append("} // namespace hc::studio::layout")
    open(HPP, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    build_skin()
    write_header()
    print("wrote", DST, "and", HPP)
