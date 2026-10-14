"""XIAO ESP32-C5 carrier for the Waveshare 3.5inch Capacitive Touch LCD (KiCad 8).

Writes the KiCad project next to this file: symbol/footprint library, schematic,
board (routed, GND pour on both layers), then runs ERC/DRC and exports
fabrication files. Run with KiCad's own Python (it has pcbnew):

    "C:/Program Files/KiCad/8.0/bin/python.exe" make_board.py

Layout (top view):
  - The XIAO sits on 1x7 pin headers (through hole), USB-C over the bottom
    edge so a case can expose it; the pin row nearest the USB is 1.27 mm from
    the edge (the header's plastic ends flush with it).
  - Its battery pads (BAT+ / BAT-, on the XIAO's underside) get a short wire
    each, out past the XIAO's far end to two holes (2.54 mm apart) that stay
    visible beside it. BAT+ goes to J2 (JST PH 2 pin).
  - J1, a 1x15 right-angle 2.54 mm header, runs up the left edge (pin 1 = VCC at
    the bottom), pins pointing out of the board, between the two left screws.
    Its order is the module's "Interface 2": VCC, 3V3, GND, MISO, MOSI, SCLK,
    SD_CS, LCD_CS, LCD_DC, LCD_RST, LCD_BL, TP_SDA, TP_SCL, TP_INT, TP_RST
    (checked on the real module 2026-10-14; its cable prints the last two as
    INT and RST).
  - XIAO pads (left column) step up-left to J1; the right column goes round the
    right side and over the top. No crossings.
Pins match fpv_config.h (FPV_PANEL_CAPTOUCH35).

XIAO geometry: Seeed's XIAO ESP32-C5 V1.1 KiCad board (pads +-7.62 mm, module
21 x 17.8 mm, BAT+ / BAT- 2.0 x 1.0 mm pads 2.0 mm apart, 4.98 mm from the
middle of the pin rows on the side away from the USB).
"""
import math
import os
import re
import shutil
import subprocess
import sys
import uuid as uuidlib

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "xiao_captouch35"
LIB = "xiao_lcd"
KICAD_CLI = shutil.which("kicad-cli") or r"C:\Program Files\KiCad\8.0\bin\kicad-cli.exe"
SYS_FP = os.environ.get("KICAD8_FOOTPRINT_DIR", r"C:\Program Files\KiCad\8.0\share\kicad\footprints")

P = 2.54

# ------------------------------------------------------------------ pinout
# LCD connector (module "Interface 2", pin 1 first) -> net
LCD_PINS = ["VCC", "3V3", "GND", "MISO", "MOSI", "SCLK", "SD_CS", "LCD_CS", "LCD_DC",
            "LCD_RST", "LCD_BL", "TP_SDA", "TP_SCL", "TP_INT", "TP_RST"]
LCD_NET = {n: n for n in LCD_PINS}
LCD_NET["VCC"] = "3V3"                       # module runs from 3.3 V (logic = supply)
for nc in ("MISO", "SD_CS", "TP_INT"):
    LCD_NET[nc] = None                       # not used by the firmware

# XIAO pin number -> (pin name, net). 1..7 = D0..D6, 8..14 = D7, D8, D9, D10,
# 3V3, GND, 5V (Seeed numbering); 15/16 = BAT+/BAT- pads on the underside.
XIAO = {
    1: ("D0", "TP_RST"),     # GPIO1
    2: ("D1", "TP_SCL"),     # GPIO0
    3: ("D2", None),         # GPIO25, strapping
    4: ("D3", None),         # GPIO7, strapping
    5: ("D4", "TP_SDA"),     # GPIO23
    6: ("D5", "LCD_BL"),     # GPIO24
    7: ("D6", "LCD_RST"),    # GPIO11
    8: ("D7", "LCD_DC"),     # GPIO12
    9: ("D8", "LCD_CS"),     # GPIO8
    10: ("D9", "SCLK"),      # GPIO9
    11: ("D10", "MOSI"),     # GPIO10
    12: ("3V3", "3V3"),
    13: ("GND", "GND"),
    14: ("5V", None),
    15: ("BAT+", "VBAT"),
    16: ("BAT-", "GND"),
}
BAT_PINS = [("1", "+", "VBAT"), ("2", "-", "GND")]

# XIAO footprint frame: USB up, pin 1 (D0) top left, origin = module centre.
XIAO_PAD_X = 7.62                    # pin rows 15.24 mm apart
XIAO_PAD_W, XIAO_PAD_H = 1.8, 1.5    # oval: 0.5 mm copper-to-edge on the row at 1.27 mm
XIAO_BAT_PADS = {15: (-0.823, 4.978), 16: (1.177, 4.973)}   # on the XIAO's underside
# board holes for the two wires: just past the XIAO's far end (it ends at 10.5),
# 2.54 mm apart, centred on the pad pair
XIAO_BAT = {15: (0.177 - 1.27, 12.6), 16: (0.177 + 1.27, 12.6)}


def uid():
    return str(uuidlib.uuid4())


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ------------------------------------------------------------------ library
def font(size=1.27):
    return f"(effects (font (size {size} {size})))"


def sym_prop(key, val, x, y, hide=False):
    h = " (hide yes)" if hide else ""
    return f'(property "{key}" "{val}" (at {x} {y} 0) (effects (font (size 1.27 1.27)){h}))'


def sym_pin(num, name, x, y, ang, length=5.08):
    return (f'(pin passive line (at {x} {y} {ang}) (length {length}) '
            f'(name "{name}" {font()}) (number "{num}" {font()}))')


def xiao_symbol(prefix=""):
    pins = []
    for i in range(7):                       # left: D0..D6 top -> bottom
        pins.append(sym_pin(i + 1, XIAO[i + 1][0], -15.24, 7.62 - P * i, 0))
    for i, num in enumerate([14, 13, 12, 11, 10, 9, 8]):   # right: 5V .. D7
        pins.append(sym_pin(num, XIAO[num][0], 15.24, 7.62 - P * i, 180))
    pins.append(sym_pin(15, "BAT+", -2.54, -15.24, 90))     # underside pads
    pins.append(sym_pin(16, "BAT-", 2.54, -15.24, 90))
    return f'''(symbol "{prefix}XIAO_ESP32C5" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)
  {sym_prop("Reference", "U", 0, 11.43)}
  {sym_prop("Value", "XIAO_ESP32C5", 0, 13.97)}
  {sym_prop("Footprint", f"{LIB}:XIAO_ESP32C5_THT_BAT", 0, -19.05, hide=True)}
  {sym_prop("Datasheet", "https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/", 0, -21.59, hide=True)}
  {sym_prop("Description", "Seeed Studio XIAO ESP32-C5 on 1x7 pin headers, battery pads wired through two holes", 0, -24.13, hide=True)}
  (symbol "XIAO_ESP32C5_0_1"
    (rectangle (start -10.16 10.16) (end 10.16 -10.16) (stroke (width 0.254) (type default)) (fill (type background))))
  (symbol "XIAO_ESP32C5_1_1"
    {chr(10).join("    " + p for p in pins)}))'''


def lcd_symbol(prefix=""):
    pins = [sym_pin(i + 1, n, -6.35, 17.78 - P * i, 0) for i, n in enumerate(LCD_PINS)]
    return f'''(symbol "{prefix}LCD_CapTouch35_Conn" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)
  {sym_prop("Reference", "J", 0, 21.59)}
  {sym_prop("Value", "LCD_CapTouch35_Conn", 0, -21.59)}
  {sym_prop("Footprint", "Connector_PinHeader_2.54mm:PinHeader_1x15_P2.54mm_Horizontal", 0, -24.13, hide=True)}
  {sym_prop("Datasheet", "https://www.waveshare.com/wiki/3.5inch_Capacitive_Touch_LCD", 0, -26.67, hide=True)}
  {sym_prop("Description", "Waveshare 3.5inch Capacitive Touch LCD, Interface 2 (15-pin) via 2.54 mm cable", 0, -29.21, hide=True)}
  (symbol "LCD_CapTouch35_Conn_0_1"
    (rectangle (start -1.27 19.05) (end 10.16 -19.05) (stroke (width 0.254) (type default)) (fill (type background))))
  (symbol "LCD_CapTouch35_Conn_1_1"
    {chr(10).join("    " + p for p in pins)}))'''


def bat_symbol(prefix=""):
    pins = [sym_pin(n, name, -6.35, 1.27 - P * i, 0) for i, (n, name, _) in enumerate(BAT_PINS)]
    return f'''(symbol "{prefix}Battery_Conn" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)
  {sym_prop("Reference", "J", 0, 3.81)}
  {sym_prop("Value", "Battery_Conn", 0, -3.81)}
  {sym_prop("Footprint", "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical", 0, -6.35, hide=True)}
  {sym_prop("Datasheet", "", 0, -8.89, hide=True)}
  {sym_prop("Description", "1-cell LiPo, JST PH 2 pin", 0, -11.43, hide=True)}
  (symbol "Battery_Conn_0_1"
    (rectangle (start -1.27 2.54) (end 3.81 -3.81) (stroke (width 0.254) (type default)) (fill (type background))))
  (symbol "Battery_Conn_1_1"
    {chr(10).join("    " + p for p in pins)}))'''


def write_symbol_lib():
    write(os.path.join(HERE, f"{LIB}.kicad_sym"),
          f'(kicad_symbol_lib (version 20231120) (generator "make_board") (generator_version "8.0")\n'
          f'{xiao_symbol()}\n{lcd_symbol()}\n{bat_symbol()}\n)\n')


def write_xiao_footprint():
    pads, labels = [], []
    rows = [(-XIAO_PAD_X, [1, 2, 3, 4, 5, 6, 7], "left"),
            (XIAO_PAD_X, [14, 13, 12, 11, 10, 9, 8], "right")]
    for x, nums, side in rows:
        for i, num in enumerate(nums):
            y = -7.62 + P * i
            shape = "rect" if num == 1 else "oval"
            pads.append(f'(pad "{num}" thru_hole {shape} (at {x} {y:.2f}) (size {XIAO_PAD_W} {XIAO_PAD_H}) '
                        f'(drill 1) (layers "*.Cu" "*.Mask"))')
            lx = -5.9 if side == "left" else 5.9
            labels.append(f'(fp_text user "{XIAO[num][0]}" (at {lx} {y:.2f} 0) (layer "F.Fab") '
                          f'(effects (font (size 0.7 0.7) (thickness 0.1)) (justify {side})))')
    for num, (x, y) in XIAO_BAT.items():
        pads.append(f'(pad "{num}" thru_hole circle (at {x} {y}) (size 1.6 1.6) (drill 1.0) '
                    f'(layers "*.Cu" "*.Mask"))')
        lx = x - 1.7 if num == 15 else x + 1.7
        for layer, mirror in (("F.SilkS", ""), ("B.SilkS", " (justify mirror)")):
            labels.append(f'(fp_text user "{XIAO[num][0][-1]}" (at {lx:.3f} {y}) (layer "{layer}") '
                          f'(effects (font (size 1 1) (thickness 0.15)){mirror}))')
    for num, (x, y) in XIAO_BAT_PADS.items():    # where the pads are, for the wiring
        labels.append(f'(fp_rect (start {x - 0.5:.3f} {y - 1.0:.3f}) (end {x + 0.5:.3f} {y + 1.0:.3f}) '
                      f'(stroke (width 0.1) (type solid)) (fill none) (layer "F.Fab"))')
    text = f'''(footprint "XIAO_ESP32C5_THT_BAT" (version 20240108) (generator "make_board") (layer "F.Cu")
  (descr "Seeed Studio XIAO ESP32-C5 on two 1x7 2.54 mm pin headers (rows 15.24 mm apart); BAT+/BAT- underside pads wired to two holes past the far end, 2.54 mm apart")
  (tags "XIAO Seeed ESP32-C5 THT battery")
  (property "Reference" "REF**" (at 0 -12.3 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))
  (property "Value" "XIAO_ESP32C5_THT_BAT" (at 0 12.3 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))
  (attr through_hole)
  (fp_poly (pts (xy -8.9 -8.6) (xy -8.9 10.5) (xy 8.9 10.5) (xy 8.9 -8.6)) (stroke (width 0.12) (type solid)) (fill none) (layer "F.SilkS"))
  (fp_rect (start -4.5 -12.1) (end 4.5 -10.5) (stroke (width 0.1) (type solid)) (fill none) (layer "F.Fab"))
  (fp_text user "USB" (at 0 -7.6 0) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))
  (fp_rect (start -8.9 -10.5) (end 8.9 10.5) (stroke (width 0.1) (type solid)) (fill none) (layer "F.Fab"))
  (fp_rect (start -9.15 -12.35) (end 9.15 13.65) (stroke (width 0.05) (type solid)) (fill none) (layer "F.CrtYd"))
  (fp_text user "${{REFERENCE}}" (at 0 0 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))
  {chr(10).join("  " + l for l in labels)}
  {chr(10).join("  " + p for p in pads)}
)
'''
    d = os.path.join(HERE, f"{LIB}.pretty")
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    write(os.path.join(d, "XIAO_ESP32C5_THT_BAT.kicad_mod"), text)


def write_lib_tables():
    write(os.path.join(HERE, "sym-lib-table"),
          f'(sym_lib_table\n  (version 7)\n  (lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.kicad_sym")(options "")(descr "project"))\n)\n')
    write(os.path.join(HERE, "fp-lib-table"),
          f'(fp_lib_table\n  (version 7)\n  (lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.pretty")(options "")(descr "project"))\n)\n')


# ------------------------------------------------------------------ schematic
def schematic():
    root = uid()
    items = []
    syms = {}

    def place(lib_id, ref, value, fp, x, y, pins, ref_dy, val_dy):
        u = uid()
        syms[ref] = u
        pin_text = " ".join(f'(pin "{p}" (uuid "{uid()}"))' for p in pins)
        items.append(f'''(symbol (lib_id "{LIB}:{lib_id}") (at {x} {y} 0) (unit 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (uuid "{u}")
  (property "Reference" "{ref}" (at {x} {y + ref_dy} 0) {font()})
  (property "Value" "{value}" (at {x} {y + val_dy} 0) {font()})
  (property "Footprint" "{fp}" (at {x} {y} 0) (effects (font (size 1.27 1.27)) (hide yes)))
  (property "Datasheet" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) (hide yes)))
  (property "Description" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) (hide yes)))
  {pin_text}
  (instances (project "{NAME}" (path "/{root}" (reference "{ref}") (unit 1)))))''')

    def label(net, x, y, ang):
        just = {180: "right bottom", 0: "left bottom", 270: "right bottom"}[ang]
        items.append(f'(label "{net}" (at {x:.2f} {y:.2f} {ang}) (effects (font (size 1.27 1.27)) (justify {just})) (uuid "{uid()}"))')

    def nc(x, y):
        items.append(f'(no_connect (at {x:.2f} {y:.2f}) (uuid "{uid()}"))')

    def pin_end(x, y, net, ang):
        label(net, x, y, ang) if net else nc(x, y)

    ux, uy = 101.6, 96.52
    place("XIAO_ESP32C5", "U1", "XIAO ESP32-C5", f"{LIB}:XIAO_ESP32C5_THT_BAT", ux, uy, range(1, 17),
          -11.43, -13.97)
    for i in range(7):
        pin_end(ux - 15.24, uy - 7.62 + P * i, XIAO[i + 1][1], 180)
    for i, num in enumerate([14, 13, 12, 11, 10, 9, 8]):
        pin_end(ux + 15.24, uy - 7.62 + P * i, XIAO[num][1], 0)
    pin_end(ux - 2.54, uy + 15.24, XIAO[15][1], 270)
    pin_end(ux + 2.54, uy + 15.24, XIAO[16][1], 270)

    jx, jy = 172.72, 101.6
    place("LCD_CapTouch35_Conn", "J1", "LCD 15P (Interface 2)",
          "Connector_PinHeader_2.54mm:PinHeader_1x15_P2.54mm_Horizontal", jx, jy, range(1, 16), -21.59, 21.59)
    for i, n in enumerate(LCD_PINS):
        pin_end(jx - 6.35, jy - 17.78 + P * i, LCD_NET[n], 180)

    bx, by = 134.62, 132.08
    place("Battery_Conn", "J2", "LiPo 1S (JST PH)",
          "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical", bx, by, ["1", "2"], -3.81, 6.35)
    for i, (_, _, net) in enumerate(BAT_PINS):
        pin_end(bx - 6.35, by - 1.27 + P * i, net, 180)

    items.append(f'(text "VCC and 3V3 both from the XIAO 3V3: the module\'s logic level follows its supply.\\n'
                 f'MISO, SD_CS, TP_INT unused (no SD card; touch is polled).\\n'
                 f'BAT+/BAT-: XIAO underside pads, wired to two holes beside the XIAO.\\n'
                 f'Pins match fpv_config.h FPV_PANEL_CAPTOUCH35." (exclude_from_sim no) (at 76.2 149.86 0) '
                 f'(effects (font (size 1.27 1.27)) (justify left bottom)) (uuid "{uid()}"))')

    libsyms = "\n".join([xiao_symbol(f"{LIB}:"), lcd_symbol(f"{LIB}:"), bat_symbol(f"{LIB}:")])
    text = f'''(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0") (uuid "{root}") (paper "A4")
  (title_block (title "XIAO ESP32-C5 - Waveshare 3.5in Capacitive Touch LCD") (rev "2") (comment 1 "fpv-lcd FPV_PANEL=CAPTOUCH35"))
  (lib_symbols
{libsyms}
  )
{chr(10).join(items)}
  (sheet_instances (path "/" (page "1")))
)
'''
    write(os.path.join(HERE, f"{NAME}.kicad_sch"), text)
    return syms


def cli(*args, check=True):
    r = subprocess.run([KICAD_CLI, *args], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if out:
        print(out)
    if check and r.returncode != 0:
        sys.exit(f"kicad-cli {args[0]} {args[1]} failed ({r.returncode})")
    return r.returncode


def read_netlist(path):
    """{(ref, pin): net name} from a kicadsexpr netlist."""
    txt = open(path, encoding="utf-8").read()
    pads = {}
    for m in re.finditer(r'\(net \(code "?\d+"?\) \(name "([^"]*)"\)(.*?)\)\s*(?=\(net \(code|\)\s*\)\s*$)', txt, re.S):
        for n in re.finditer(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', m.group(2)):
            pads[(n.group(1), n.group(2))] = m.group(1)
    return pads


# ------------------------------------------------------------------ board
# Board coordinates below are mm from the bottom-left corner, y up.
W, H = 43.0, 54.0
X0, Y0 = 100.0, 80.0                   # top-left corner on the KiCad sheet
EDGE_PIN = 1.27                        # XIAO pad row nearest the USB, from the bottom edge
XC, YC = 21.0, EDGE_PIN + 7.62         # XIAO centre (USB down)
HX, HY1 = 4.04, 9.0                    # J1 pad column; pin 1 height. Body flush with the left edge
HOLES = [(3.5, 3.5), (W - 3.5, 3.5), (3.5, H - 3.5), (W - 3.5, H - 3.5)]
HOLE_KEEPOUT = 3.2
W_SIG, W_PWR = 0.3, 0.6


def mm(v):
    return pcbnew.FromMM(v)


def at(x, y):
    """board mm (y up from the bottom edge) -> KiCad position"""
    return pcbnew.VECTOR2I(mm(X0 + x), mm(Y0 + H - y))


def hdr_y(pin):
    return HY1 + P * (pin - 1)


def xiao_left(i):      # USB down: left column bottom -> top = 5V, GND, 3V3, D10, D9, D8, D7
    return XC - XIAO_PAD_X, EDGE_PIN + P * i


def xiao_right(i):     # right column bottom -> top = D0 .. D6
    return XC + XIAO_PAD_X, EDGE_PIN + P * i


def build_board(syms, pads):
    path = os.path.join(HERE, f"{NAME}.kicad_pcb")
    board = pcbnew.NewBoard(path)
    board.GetDesignSettings().SetCopperLayerCount(2)

    nets = {}
    for name in sorted(set(pads.values())):
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        nets[name] = ni
    by_short = {k.lstrip("/"): v for k, v in nets.items()}

    def footprint(libdir, lib, fpname, ref, value, x, y, rot, sym_uuid=None):
        fp = pcbnew.FootprintLoad(libdir, fpname)
        fp.SetFPID(pcbnew.LIB_ID(lib, fpname))
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.SetPosition(at(x, y))
        fp.SetOrientationDegrees(rot)
        if sym_uuid:
            fp.SetPath(pcbnew.KIID_PATH(f"/{sym_uuid}"))
            fp.SetSheetname("Root")
            fp.SetSheetfile(f"{NAME}.kicad_sch")
        board.Add(fp)
        for pad in fp.Pads():
            n = pads.get((ref, pad.GetNumber()))
            if n:
                pad.SetNet(nets[n])
            if (ref, pad.GetNumber()) == ("J1", "3"):     # GND: solid, the F.Cu sliver there is tiny
                pad.SetZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
        return fp

    u1 = footprint(os.path.join(HERE, f"{LIB}.pretty"), LIB, "XIAO_ESP32C5_THT_BAT", "U1",
                   "XIAO ESP32-C5", XC, YC, 180, syms["U1"])
    j1 = footprint(os.path.join(SYS_FP, "Connector_PinHeader_2.54mm.pretty"), "Connector_PinHeader_2.54mm",
                   "PinHeader_1x15_P2.54mm_Horizontal", "J1", "LCD 15P (Interface 2)", HX, HY1, 180, syms["J1"])
    jb = (XC - XIAO_BAT[15][0], 27.0)   # J2 pin 1 straight above the BAT+ hole
    j2 = footprint(os.path.join(SYS_FP, "Connector_JST.pretty"), "Connector_JST",
                   "JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical", "J2", "LiPo 1S (JST PH)", jb[0], jb[1], 0, syms["J2"])
    for i, (x, y) in enumerate(HOLES):
        h = footprint(os.path.join(SYS_FP, "MountingHole.pretty"), "MountingHole", "MountingHole_3.2mm_M3",
                      f"H{i + 1}", "M3", x, y, 0)
        h.SetBoardOnly(True)
        h.SetExcludedFromBOM(True)
        h.SetExcludedFromPosFiles(True)
        h.Reference().SetVisible(False)

    # placement matches the routing plan
    def pad_at(fp, num):
        p = fp.FindPadByNumber(str(num)).GetPosition()
        return round(pcbnew.ToMM(p.x) - X0, 2), round(H - (pcbnew.ToMM(p.y) - Y0), 2)

    def same(a, b):
        return abs(a[0] - b[0]) < 0.01 and abs(a[1] - b[1]) < 0.01

    for i, num in enumerate([14, 13, 12, 11, 10, 9, 8]):
        assert same(pad_at(u1, num), xiao_left(i)), (num, pad_at(u1, num))
    for i in range(7):
        assert same(pad_at(u1, i + 1), xiao_right(i)), (i + 1, pad_at(u1, i + 1))
    bat_plus = pad_at(u1, 15)
    assert same(bat_plus, (XC - XIAO_BAT[15][0], YC + XIAO_BAT[15][1])), bat_plus
    assert bat_plus[1] - 0.8 > YC + 10.5, "BAT holes must clear the XIAO"
    for pin in range(1, 16):
        assert same(pad_at(j1, pin), (HX, hdr_y(pin))), (pin, pad_at(j1, pin))
    assert same(pad_at(j2, 1), jb)

    u1.Reference().SetPosition(at(XC, YC + 1.5))
    u1.Reference().SetTextAngleDegrees(0)
    u1.Value().SetVisible(False)
    j1.Reference().SetPosition(at(HX + 1.2, HY1 - 2.6))
    j1.Reference().SetTextAngleDegrees(0)
    j1.Value().SetVisible(False)
    j2.Reference().SetPosition(at(jb[0] + 1.0, jb[1] + 4.3))
    # J1: its pins stick out of the board; the silk drawn past the edge goes to F.Fab
    for item in j1.GraphicalItems():
        if item.GetLayer() == pcbnew.F_SilkS and item.GetBoundingBox().GetLeft() < mm(X0 + 0.3):
            item.SetLayer(pcbnew.F_Fab)
    j2.Value().SetVisible(False)

    # ---- tracks
    F, B = pcbnew.F_Cu, pcbnew.B_Cu

    def route(net, layer, points, width=W_SIG):
        for (xa, ya), (xb, yb) in zip(points, points[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(at(xa, ya))
            t.SetEnd(at(xb, yb))
            t.SetWidth(mm(width))
            t.SetLayer(layer)
            t.SetNet(by_short[net])
            board.Add(t)

    def via(net, x, y):
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(at(x, y))
        v.SetWidth(mm(0.6))
        v.SetDrill(mm(0.3))
        v.SetNet(by_short[net])
        board.Add(v)

    def staircase(net, x0, y0, xv, ty, c=0.8):
        """pad (x0, y0) -> left to xv -> up to ty -> left to J1, 45-degree corners"""
        route(net, F, [(x0, y0), (xv + c, y0), (xv, y0 + c), (xv, ty - c), (xv - c, ty), (HX, ty)])

    def round_right(net, x0, y0, xv, ty, c=0.8):
        """pad (x0, y0) -> right to xv -> up to ty -> left to J1"""
        route(net, F, [(x0, y0), (xv - c, y0), (xv, y0 + c), (xv, ty - c), (xv - c, ty), (HX, ty)])

    # left column: D10 -> MOSI, D9 -> SCLK, D8 -> LCD_CS, D7 -> LCD_DC (lower pad = further left)
    for i, pin, xv in ((3, 5, 7.0), (4, 6, 8.0), (5, 8, 9.0), (6, 9, 10.0)):
        x, y = xiao_left(i)
        staircase(LCD_PINS[pin - 1], x, y, xv, hdr_y(pin))
    # right column round the right side: D0 outermost -> TP_RST (top) ... D6 innermost -> LCD_RST
    for i, pin, xv in ((0, 15, 35.0), (1, 13, 34.0), (4, 12, 33.0), (5, 11, 32.0), (6, 10, 31.0)):
        x, y = xiao_right(i)
        round_right(LCD_NET[LCD_PINS[pin - 1]], x, y, xv, hdr_y(pin))
    # 3V3: XIAO 3V3 -> J1 pin 1 (VCC) and pin 2 (3V3)
    x, y = xiao_left(2)
    route("3V3", F, [(x, y), (7.6, y), (5.6, hdr_y(1) - 0.1), (HX, hdr_y(1))], W_PWR)
    route("3V3", F, [(HX, hdr_y(1)), (HX, hdr_y(2))], W_PWR)
    # BAT+ (hole under the XIAO) -> J2 pin 1, on B.Cu
    route("VBAT", B, [bat_plus, jb], 0.8)
    # GND stitching vias around the board
    for x, y in ((14.0, 23.0), (28.5, 23.0), (38.0, 12.0), (38.0, 30.0), (20.0, 47.0), (30.0, 47.0),
                 (12.0, 47.0), (38.0, 40.0)):
        via("GND", x, y)

    # ---- outline (rounded corners)
    r = 2.0

    def seg(a, b):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(at(*a)); s.SetEnd(at(*b))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1))
        board.Add(s)

    def arc(cx, cy, sx, sy, ex, ey):
        dx, dy = (sx - cx) + (ex - cx), (sy - cy) + (ey - cy)
        k = r / math.hypot(dx, dy)
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(at(sx, sy), at(cx + dx * k, cy + dy * k), at(ex, ey))
        s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1))
        board.Add(s)

    seg((r, 0), (W - r, 0)); seg((W, r), (W, H - r)); seg((W - r, H), (r, H)); seg((0, H - r), (0, r))
    arc(W - r, r, W - r, 0, W, r)
    arc(W - r, H - r, W, H - r, W - r, H)
    arc(r, H - r, r, H, 0, H - r)
    arc(r, r, 0, r, r, 0)

    # ---- GND pour, both layers
    for layer in (F, B):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(by_short["GND"])
        z.SetLocalClearance(mm(0.3))
        z.SetMinThickness(mm(0.25))
        z.SetThermalReliefGap(mm(0.5))
        z.SetThermalReliefSpokeWidth(mm(0.5))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in ((0, 0), (W, 0), (W, H), (0, H)):
            ol.Append(at(x, y).x, at(x, y).y)
        board.Add(z)

    def keepout(points, layers, tracks=False):
        k = pcbnew.ZONE(board)
        k.SetIsRuleArea(True)
        k.SetDoNotAllowCopperPour(True)
        k.SetDoNotAllowTracks(tracks)
        k.SetDoNotAllowVias(True)
        k.SetDoNotAllowPads(False)
        k.SetDoNotAllowFootprints(False)
        k.SetLayerSet(layers)
        ko = k.Outline()
        ko.NewOutline()
        for x, y in points:
            p = at(x, y)
            ko.Append(p.x, p.y)
        board.Add(k)

    # screw holes: no copper
    for hx, hy in HOLES:
        keepout([(hx + HOLE_KEEPOUT * math.cos(math.radians(a)), hy + HOLE_KEEPOUT * math.sin(math.radians(a)))
                 for a in range(0, 360, 15)], pcbnew.LSET.AllCuMask(), tracks=True)

    # ---- silkscreen
    def text(s, x, y, size=1.0, layer=pcbnew.F_SilkS, angle=0, just=None, thick=None):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s)
        t.SetPosition(at(x, y))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(thick or size * 0.15))
        t.SetTextAngleDegrees(angle)
        if just == "right":
            t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
        elif just == "left":
            t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
        if layer == pcbnew.B_SilkS:
            t.SetMirrored(True)
        board.Add(t)

    for pin, name in enumerate(LCD_PINS, 1):
        text(name, HX + 1.25, hdr_y(pin) + 0.75, 0.8, just="left", thick=0.12)
    text("+", jb[0] - 3.2, jb[1], 1.2)
    text("-", jb[0] + 5.2, jb[1], 1.2)
    text("fpv-lcd CAPTOUCH35 rev2", 22.0, 44.0, 1.0, layer=pcbnew.B_SilkS)
    text("LCD Interface 2: pin 1 = VCC (bottom)", 22.0, 41.5, 0.9, layer=pcbnew.B_SilkS)
    text("BAT: wires from the XIAO pads", XC, YC + 14.8, 0.9, layer=pcbnew.B_SilkS)

    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    pcbnew.SaveBoard(path, board)
    return path


def main():
    write_symbol_lib()
    write_xiao_footprint()
    write_lib_tables()
    syms = schematic()
    sch = os.path.join(HERE, f"{NAME}.kicad_sch")
    out = os.path.join(HERE, "out")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    net = os.path.join(out, f"{NAME}.net")
    cli("sch", "export", "netlist", "--format", "kicadsexpr", "-o", net, sch)
    pads = read_netlist(net)
    assert len(pads) == 33, f"expected 33 pins in the netlist, got {len(pads)}"
    pcb = build_board(syms, pads)

    cli("sch", "erc", "--exit-code-violations", "-o", os.path.join(out, "erc.rpt"), sch, check=False)
    cli("pcb", "drc", "--schematic-parity", "--exit-code-violations", "-o", os.path.join(out, "drc.rpt"),
        pcb, check=False)
    cli("sch", "export", "pdf", "-o", os.path.join(out, f"{NAME}_schematic.pdf"), sch)
    cli("pcb", "export", "pdf", "--layers", "F.Cu,F.SilkS,Edge.Cuts", "--include-border-title",
        "-o", os.path.join(out, f"{NAME}_top.pdf"), pcb)
    cli("pcb", "export", "pdf", "--layers", "B.Cu,B.SilkS,Edge.Cuts", "--mirror",
        "-o", os.path.join(out, f"{NAME}_bottom.pdf"), pcb)
    cli("pcb", "export", "svg", "--layers", "F.Cu,B.Cu,F.SilkS,Edge.Cuts", "--exclude-drawing-sheet",
        "--page-size-mode", "2", "-o", os.path.join(out, f"{NAME}_preview.svg"), pcb)
    gerb = os.path.join(out, "gerber")
    os.makedirs(gerb, exist_ok=True)
    cli("pcb", "export", "gerbers", "--layers",
        "F.Cu,B.Cu,F.SilkS,B.SilkS,F.Mask,B.Mask,F.Paste,Edge.Cuts", "-o", gerb + os.sep, pcb)
    cli("pcb", "export", "drill", "--format", "excellon", "--excellon-separate-th", "-o", gerb + os.sep, pcb)
    shutil.make_archive(os.path.join(out, f"{NAME}_gerber"), "zip", gerb)
    print("done:", pcb)


if __name__ == "__main__":
    main()
