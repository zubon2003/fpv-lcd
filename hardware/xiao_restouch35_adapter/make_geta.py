"""XIAO ESP32-C5 -> Waveshare Pico-ResTouch-LCD-3.5 adapter (下駄基板).

The board has the Raspberry Pi Pico's outline and header positions (it plugs
into the module's Pico sockets like a Pico) plus an extension past the Pico's
USB end that carries the XIAO, its USB-C at the board edge.

Single copper layer (bottom), every net routed without crossings, so it can be
milled or etched at home. Only the Pico pins that carry a signal get a pad.

Coordinates: mm, seen from the top (component side), X right, Y down from the
Pico's USB end; the DXF flips Y so the drawing looks the same.

    python3 make_geta.py out_dir
"""
import json
import sys
from pathlib import Path

import ezdxf
from shapely.geometry import LineString, Point, box
from shapely.ops import unary_union

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
OUT.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------- geometry
P = 2.54
BOARD_W = 21.0                 # Pico width
PICO_L = 51.0                  # Pico length
EXT = 24.0                     # extension for the XIAO (past the Pico's USB end)
TRACK = 0.6                    # track width
PAD_D = 1.8                    # pad diameter
DRILL = 1.0                    # header pin hole
WIRE_PAD_D = 2.0
WIRE_DRILL = 0.9

# Pico: rows 17.78 mm apart, 1.61 mm in from the long edges; pin 1 is 1.37 mm
# from the USB end. Pins 1..20 down the left side, 21..40 up the right side.
PICO_X_L, PICO_X_R = 1.61, BOARD_W - 1.61


def pico_pin(n):
    if n <= 20:
        return (PICO_X_L, 1.37 + (n - 1) * P)
    return (PICO_X_R, 1.37 + (40 - n) * P)


# XIAO (21 x 17.8 mm), USB-C at the top edge of the extension, face up.
# Rows 15.24 mm apart, centred on the board. Left column D0..D6 from the USB
# end; right column 5V, GND, 3V3, D10, D9, D8, D7.
XIAO_TOP = -EXT
XIAO_W, XIAO_L = 17.8, 21.0
XIAO_X_L, XIAO_X_R = BOARD_W / 2 - 3 * P, BOARD_W / 2 + 3 * P
XIAO_Y0 = XIAO_TOP + (XIAO_L - 6 * P) / 2
XIAO_LEFT = ["D0", "D1", "D2", "D3", "D4", "D5", "D6"]
XIAO_RIGHT = ["5V", "GND", "3V3", "D10", "D9", "D8", "D7"]
XIAO_GPIO = {"D0": 1, "D1": 0, "D2": 25, "D3": 7, "D4": 23, "D5": 24, "D6": 11,
             "D7": 12, "D8": 8, "D9": 9, "D10": 10}


def xiao_pin(name):
    if name in XIAO_LEFT:
        return (XIAO_X_L, XIAO_Y0 + XIAO_LEFT.index(name) * P)
    return (XIAO_X_R, XIAO_Y0 + XIAO_RIGHT.index(name) * P)


# Nets: XIAO pin -> Pico pin (module signal). The order is what lets one
# layer carry everything: in the channel between the Pico rows the tracks run
# left to right in the order of the Pico pins they end on.
NETS = [
    # xiao, pico pin, module signal, channel x
    ("D6", 11, "LCD_DC", 3.6),
    ("D5", 12, "LCD_CS", 4.8),
    ("D4", 14, "LCD_CLK", 6.0),
    ("D1", 15, "MOSI", 7.2),
    ("D0", 16, "MISO", 8.4),
    ("D10", 17, "LCD_BL", 9.6),
    ("D9", 20, "LCD_RST", 10.8),
    ("D7", 21, "TP_CS", 14.0),
]
GND_X, GND_LANE = 17.4, BOARD_W - 1.1     # GND: outside the XIAO's right row
GND_PICO = [38, 28]
VSYS_PICO = 39
VSYS_WIRE = (PICO_X_R, 0.9)               # wire pad, where the unused pin 40 would be
V5_WIRE = (14.6, xiao_pin("5V")[1])       # wire pad on the XIAO 5V pin

tracks = []      # (name, [points])
for xp, pp, sig, cx in NETS:
    sx, sy = xiao_pin(xp)
    dx, dy = pico_pin(pp)
    if xp == "D6":                       # lowest left pin: straight down, then into its lane
        pts = [(sx, sy), (sx, sy + 1.6), (cx, sy + 2.4), (cx, dy), (dx, dy)]
    else:
        pts = [(sx, sy), (cx, sy), (cx, dy), (dx, dy)]
    tracks.append((f"{xp}-{sig}", pts))

gx, gy = xiao_pin("GND")
g_end = pico_pin(GND_PICO[-1])
tracks.append(("GND", [(gx, gy), (GND_LANE, gy), (GND_LANE, -1.4), (GND_X, -1.4), (GND_X, g_end[1]), g_end]))
for n in GND_PICO[:-1]:
    px, py = pico_pin(n)
    tracks.append(("GND", [(GND_X, py), (px, py)]))
tracks.append(("VSYS", [VSYS_WIRE, pico_pin(VSYS_PICO)]))
tracks.append(("5V", [V5_WIRE, xiao_pin("5V")]))

pads = []        # (x, y, diameter, drill, label, kind)
for name in XIAO_LEFT + XIAO_RIGHT:
    x, y = xiao_pin(name)
    pads.append((x, y, PAD_D, DRILL, name, "xiao"))
pico_used = sorted({pp for _, pp, _, _ in NETS} | set(GND_PICO) | {VSYS_PICO})
for n in pico_used:
    x, y = pico_pin(n)
    pads.append((x, y, PAD_D, DRILL, str(n), "pico"))
pads.append((*VSYS_WIRE, WIRE_PAD_D, WIRE_DRILL, "VSYS", "wire"))
pads.append((*V5_WIRE, WIRE_PAD_D, WIRE_DRILL, "5V", "wire"))

outline = box(0, -EXT, BOARD_W, PICO_L)

# ---------------------------------------------------------------- checks
CLEAR = 0.4
shapes = []      # (net, geometry)
net_of_pad = {}
for x, y, d, _, label, kind in pads:
    net = None
    for tn, pts in tracks:
        if Point(pts[0]).distance(Point(x, y)) < 1e-6 or Point(pts[-1]).distance(Point(x, y)) < 1e-6:
            net = tn
    if kind == "pico" and int(label) in GND_PICO:
        net = "GND"
    net_of_pad[(x, y)] = net or f"nc:{kind}:{label}"
    shapes.append((net_of_pad[(x, y)], Point(x, y).buffer(d / 2, 32)))
for tn, pts in tracks:
    shapes.append((tn, LineString(pts).buffer(TRACK / 2, 16)))

problems = []
for i in range(len(shapes)):
    for j in range(i + 1, len(shapes)):
        a, b = shapes[i], shapes[j]
        if a[0] == b[0]:
            continue
        d = a[1].distance(b[1])
        if d < CLEAR - 1e-6:
            problems.append((a[0], b[0], round(d, 3)))
edge = outline.exterior
for n, g in shapes:
    d = g.distance(edge)
    if d < 0.5 - 1e-6 or not outline.contains(g):
        problems.append((n, "board edge", round(d, 3)))
copper = unary_union([g for _, g in shapes])

# ---------------------------------------------------------------- DXF
def flip(p):
    return (p[0], -p[1])


doc = ezdxf.new("R2010", setup=True)
doc.units = ezdxf.units.MM
for name, color in (("OUTLINE", 7), ("COPPER_BOTTOM", 1), ("TRACK_CENTER", 3), ("DRILL", 5),
                    ("SILK_TOP", 2), ("NOTE", 8)):
    doc.layers.add(name, color=color)
msp = doc.modelspace()
msp.add_lwpolyline([flip(p) for p in outline.exterior.coords], close=True, dxfattribs={"layer": "OUTLINE"})

geoms = list(copper.geoms) if copper.geom_type == "MultiPolygon" else [copper]
for g in geoms:
    msp.add_lwpolyline([flip(p) for p in g.exterior.coords], close=True, dxfattribs={"layer": "COPPER_BOTTOM"})
    for hole in g.interiors:
        msp.add_lwpolyline([flip(p) for p in hole.coords], close=True, dxfattribs={"layer": "COPPER_BOTTOM"})
for tn, pts in tracks:
    msp.add_lwpolyline([flip(p) for p in pts], dxfattribs={"layer": "TRACK_CENTER", "const_width": TRACK})
for x, y, d, drill, label, kind in pads:
    msp.add_circle(flip((x, y)), drill / 2, dxfattribs={"layer": "DRILL"})


def text(s, p, h=0.9, align="MIDDLE_CENTER", layer="SILK_TOP"):
    t = msp.add_text(s, height=h, dxfattribs={"layer": layer})
    t.set_placement(flip(p), align=getattr(ezdxf.enums.TextEntityAlignment, align))


# silk: XIAO body, Pico outline, labels
xb = (BOARD_W / 2 - XIAO_W / 2, XIAO_TOP, BOARD_W / 2 + XIAO_W / 2, XIAO_TOP + XIAO_L)
msp.add_lwpolyline([flip(p) for p in [(xb[0], xb[1]), (xb[2], xb[1]), (xb[2], xb[3]), (xb[0], xb[3])]],
                   close=True, dxfattribs={"layer": "SILK_TOP"})
text("XIAO C5", (BOARD_W / 2, XIAO_TOP + 1.6), 0.9)
text("face up", (BOARD_W / 2, XIAO_TOP + 2.8), 0.7)
msp.add_line(flip((0, 0)), flip((BOARD_W, 0)), dxfattribs={"layer": "SILK_TOP", "linetype": "DASHED"})
text("Pico USB end", (0.6, -0.6), 0.6, "MIDDLE_LEFT")
for x, y, d, drill, label, kind in pads:
    if kind == "xiao":
        off = 1.9 if x < BOARD_W / 2 else -1.9
        text(label, (x + off, y), 0.7)
    elif kind == "pico":
        off = 1.9 if x < BOARD_W / 2 else -1.9
        sig = {pp: s for _, pp, s, _ in NETS}.get(int(label), "GND" if int(label) in GND_PICO else "VSYS")
        text(f"{label} {sig}", (x + off, y), 0.6, "MIDDLE_LEFT" if off > 0 else "MIDDLE_RIGHT")
    else:
        text(label, (x - 1.4, y if label == "5V" else y - 0.9), 0.6, "MIDDLE_RIGHT")
text("wire: 5V or BAT+", (3.6 + 1.0, 9.0), 0.6, "MIDDLE_LEFT")
text("  -> VSYS pad", (3.6 + 1.0, 10.0), 0.6, "MIDDLE_LEFT")
text("copper: bottom (drawn as seen from the top)", (BOARD_W / 2, PICO_L + 2), 0.8, layer="NOTE")
doc.saveas(OUT / "xiao_restouch35_adapter.dxf")

# bottom copper mirrored (as seen from the copper side: for toner transfer / film)
doc2 = ezdxf.new("R2010", setup=True)
doc2.units = ezdxf.units.MM
doc2.layers.add("COPPER_BOTTOM_MIRRORED", color=1)
doc2.layers.add("OUTLINE", color=7)
doc2.layers.add("DRILL", color=5)
m2 = doc2.modelspace()
mir = lambda p: (BOARD_W - p[0], -p[1])  # noqa: E731
m2.add_lwpolyline([mir(p) for p in outline.exterior.coords], close=True, dxfattribs={"layer": "OUTLINE"})
for g in geoms:
    m2.add_lwpolyline([mir(p) for p in g.exterior.coords], close=True, dxfattribs={"layer": "COPPER_BOTTOM_MIRRORED"})
    for hole in g.interiors:
        m2.add_lwpolyline([mir(p) for p in hole.coords], close=True, dxfattribs={"layer": "COPPER_BOTTOM_MIRRORED"})
for x, y, d, drill, label, kind in pads:
    m2.add_circle(mir((x, y)), drill / 2, dxfattribs={"layer": "DRILL"})
doc2.saveas(OUT / "xiao_restouch35_adapter_bottom_mirrored.dxf")

# ---------------------------------------------------------------- PNG
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
MARGIN = 2.0                               # mm around the board


def raster(px_per_mm, mirror=False):
    w = int(round((BOARD_W + 2 * MARGIN) * px_per_mm))
    h = int(round((PICO_L + EXT + 2 * MARGIN) * px_per_mm))

    def T(p):
        x, y = p
        if mirror:
            x = BOARD_W - x
        return ((x + MARGIN) * px_per_mm, (y + EXT + MARGIN) * px_per_mm)
    return w, h, T


def draw_polys(d, T, polys, fill, hole_fill):
    for g in polys:
        d.polygon([T(p) for p in g.exterior.coords], fill=fill)
        for hole in g.interiors:
            d.polygon([T(p) for p in hole.coords], fill=hole_fill)


def font(px):
    return ImageFont.truetype(FONT, max(8, int(px)))


def sig_of(label):
    return {pp: s for _, pp, s, _ in NETS}.get(int(label), "GND" if int(label) in GND_PICO else "VSYS")


def render_overview(path, k=24):
    w, h, T = raster(k)
    im = Image.new("RGB", (w, h + 3 * k), "#101418")
    d = ImageDraw.Draw(im)
    d.rectangle([T((0, -EXT)), T((BOARD_W, PICO_L))], fill="#1d6b3a", outline="#9fd8b0", width=2)
    draw_polys(d, T, geoms, "#d9a441", "#1d6b3a")
    for x, y, dd, drill, label, kind in pads:
        cx, cy = T((x, y))
        r = drill / 2 * k
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill="#101418")
    # XIAO body and the Pico USB end
    d.rectangle([T(xb[:2]), T(xb[2:])], outline="white", width=2)
    yx = T((0, 0))[1]
    for x0 in range(0, int(BOARD_W * 10), 8):
        d.line([T((x0 / 10, 0)), T((min(x0 / 10 + 0.4, BOARD_W), 0))], fill="white", width=1)
    f = font(0.75 * k)
    fs = font(0.6 * k)
    for x, y, dd, drill, label, kind in pads:
        cx, cy = T((x, y))
        left = x < BOARD_W / 2
        if kind == "xiao":
            g = XIAO_GPIO.get(label)
            t = label + (f" (G{g})" if g is not None else "")
        elif kind == "pico":
            t = f"{label} {sig_of(label)}"
        else:
            t = label + " pad"
            left = False
        off = 1.25 * k
        if left:
            d.text((cx + off, cy), t, fill="white", font=f, anchor="lm")
        else:
            d.text((cx - off, cy), t, fill="white", font=f, anchor="rm")
    d.text(T((BOARD_W / 2, -EXT + 1.6)), "XIAO ESP32-C5 (face up)", fill="white", font=fs, anchor="mm")
    d.text(T((BOARD_W / 2, -EXT - 1.0)), "USB-C", fill="white", font=fs, anchor="mm")
    d.text(T((0.6, -0.7)), "Pico USB end", fill="white", font=fs, anchor="lm")
    d.text(T((4.6, 9.0)), "wire 5V pad (USB) or", fill="#ffe08a", font=fs, anchor="lm")
    d.text(T((4.6, 10.0)), "XIAO BAT+ -> VSYS pad", fill="#ffe08a", font=fs, anchor="lm")
    d.text((w / 2, h + 1.5 * k), f"top view, copper on the bottom   {BOARD_W:g} x {PICO_L + EXT:g} mm",
           fill="white", font=fs, anchor="mm")
    im.save(path)


def render_mask(path, mirror, dpi=600):
    k = dpi / 25.4
    w, h, T = raster(k, mirror)
    im = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(im)
    draw_polys(d, T, geoms, 0, 255)
    for x, y, dd, drill, label, kind in pads:
        cx, cy = T((x, y))
        r = drill / 2 * k * 0.5           # small centre mark for drilling
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    (x0, y0), (x1, y1) = T((0, -EXT)), T((BOARD_W, PICO_L))
    d.rectangle([min(x0, x1), y0, max(x0, x1), y1], outline=0, width=max(1, int(0.1 * k)))
    im.save(path, dpi=(dpi, dpi))


def render_silk(path, k=24):
    w, h, T = raster(k)
    im = Image.new("RGB", (w, h), "white")
    d = ImageDraw.Draw(im)
    d.rectangle([T((0, -EXT)), T((BOARD_W, PICO_L))], outline="black", width=2)
    d.rectangle([T(xb[:2]), T(xb[2:])], outline="black", width=2)
    f = font(0.7 * k)
    for x, y, dd, drill, label, kind in pads:
        cx, cy = T((x, y))
        r = dd / 2 * k
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline="black", width=2)
        t = (label if kind == "xiao" else f"{label} {sig_of(label)}" if kind == "pico" else label)
        if x < BOARD_W / 2 and kind != "wire":
            d.text((cx + 1.2 * k, cy), t, fill="black", font=f, anchor="lm")
        else:
            d.text((cx - 1.2 * k, cy), t, fill="black", font=f, anchor="rm")
    d.text(T((BOARD_W / 2, -EXT + 1.6)), "XIAO ESP32-C5 (face up)", fill="black", font=f, anchor="mm")
    im.save(path)


render_overview(OUT / "xiao_restouch35_adapter_preview.png")
render_mask(OUT / "copper_top_view_1to1_600dpi.png", mirror=False)
render_mask(OUT / "copper_mirrored_1to1_600dpi.png", mirror=True)
render_silk(OUT / "silk_top.png")

report = {
    "board_mm": [BOARD_W, PICO_L + EXT],
    "tracks": len(tracks),
    "pads": len(pads),
    "clearance_problems": problems,
    "nets": [{"xiao": xp, "gpio": XIAO_GPIO[xp], "pico_pin": pp, "signal": s} for xp, pp, s, _ in NETS],
}
(OUT / "report.json").write_text(json.dumps(report, indent=1))
print(json.dumps({k: report[k] for k in ("board_mm", "tracks", "pads", "clearance_problems")}))
