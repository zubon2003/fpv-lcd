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
text("XIAO ESP32-C5  (USB-C ->)", (BOARD_W / 2, XIAO_TOP + 9.5), 0.9)
text("face up", (BOARD_W / 2, XIAO_TOP + 11), 0.8)
msp.add_line(flip((0, 0)), flip((BOARD_W, 0)), dxfattribs={"layer": "SILK_TOP", "linetype": "DASHED"})
text("Pico USB end (pin 1 left)", (BOARD_W / 2, 0.9), 0.7)
for x, y, d, drill, label, kind in pads:
    if kind == "xiao":
        off = 1.9 if x < BOARD_W / 2 else -1.9
        text(label, (x + off, y), 0.7)
    elif kind == "pico":
        off = 1.9 if x < BOARD_W / 2 else -1.9
        sig = {pp: s for _, pp, s, _ in NETS}.get(int(label), "GND" if int(label) in GND_PICO else "VSYS")
        text(f"{label} {sig}", (x + off, y), 0.6, "MIDDLE_LEFT" if off > 0 else "MIDDLE_RIGHT")
    else:
        text(label, (x - 2.0, y), 0.7, "MIDDLE_RIGHT")
text("wire 5V or BAT+ -> VSYS", (BOARD_W / 2, -0.6 + 4.6), 0.6)
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

# ---------------------------------------------------------------- preview
import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

fig, ax = plt.subplots(figsize=(5.2, 9.5), dpi=150)
ax.set_facecolor("#0b3d1e")
xs, ys = outline.exterior.xy
ax.fill(xs, [-v for v in ys], color="#1f6b38", zorder=0)
for g in geoms:
    x, y = g.exterior.xy
    ax.fill(x, [-v for v in y], color="#d9a441", zorder=1)
for x, y, d, drill, label, kind in pads:
    ax.add_patch(plt.Circle((x, -y), drill / 2, color="#111", zorder=3))
ax.add_patch(plt.Rectangle((xb[0], -xb[3]), XIAO_W, XIAO_L, fill=False, ec="white", ls="--", lw=0.8, zorder=4))
ax.plot([0, BOARD_W], [0, 0], color="white", ls=":", lw=0.6)
for x, y, d, drill, label, kind in pads:
    if kind == "pico":
        sig = {pp: s for _, pp, s, _ in NETS}.get(int(label), "GND" if int(label) in GND_PICO else "VSYS")
        lab, ha, dx = f"{label} {sig}", ("left" if x < BOARD_W / 2 else "right"), (1.2 if x < BOARD_W / 2 else -1.2)
    elif kind == "xiao":
        g = XIAO_GPIO.get(label)
        lab = label + (f" G{g}" if g is not None else "")
        ha, dx = ("left" if x < BOARD_W / 2 else "right"), (1.2 if x < BOARD_W / 2 else -1.2)
    else:
        lab, ha, dx = label, "right", -1.3
    ax.text(x + dx, -y, lab, color="white", fontsize=5, ha=ha, va="center", zorder=5)
ax.text(BOARD_W / 2, -XIAO_TOP - 0.2, "USB-C", color="white", fontsize=6, ha="center", va="bottom")
ax.text(BOARD_W / 2, 0.4, "Pico USB end", color="white", fontsize=5, ha="center", va="bottom")
ax.set_aspect("equal")
ax.set_xlim(-1.5, BOARD_W + 1.5)
ax.set_ylim(-PICO_L - 1.5, EXT + 2.5)
ax.set_title("XIAO ESP32-C5 -> Pico-ResTouch-LCD-3.5\n(top view, copper on the bottom)", fontsize=8)
ax.set_xticks([])
ax.set_yticks([])
fig.tight_layout()
fig.savefig(OUT / "xiao_restouch35_adapter_preview.png")

report = {
    "board_mm": [BOARD_W, PICO_L + EXT],
    "tracks": len(tracks),
    "pads": len(pads),
    "clearance_problems": problems,
    "nets": [{"xiao": xp, "gpio": XIAO_GPIO[xp], "pico_pin": pp, "signal": s} for xp, pp, s, _ in NETS],
}
(OUT / "report.json").write_text(json.dumps(report, indent=1))
print(json.dumps({k: report[k] for k in ("board_mm", "tracks", "pads", "clearance_problems")}))
