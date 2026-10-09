"""Count the RV32IMAC instructions the firmware DSP executes (bench.c under
the Unicorn emulator) and turn them into ESP32-C5 time with a simple cycle
model, then simulate the sweep and screen rates of each mode.

    python3 cycles.py /path/to/fpv-band-monitor-c5
"""
import sys
from pathlib import Path

import numpy as np
from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_RISCV, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_RISCV32, Uc
from unicorn.riscv_const import UC_RISCV_REG_PC

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.argv += [] if len(sys.argv) > 1 else ["/tmp/fpv-band-monitor-c5"]
import check_dsp as cd  # noqa: E402

F_CPU = 240e6


def classify(insn, size):
    """-> (kind) one of alu, load, store, mul, div, branch, jump"""
    if size == 2:
        q, f3 = insn & 3, (insn >> 13) & 7
        if q == 0:
            return "load" if f3 in (2, 3) else "store" if f3 in (6, 7) else "alu"
        if q == 1:
            return "branch" if f3 in (6, 7) else "jump" if f3 in (1, 5) else "alu"
        if f3 == 2:
            return "load"
        if f3 == 6:
            return "store"
        if f3 == 4 and ((insn >> 2) & 31) == 0 and ((insn >> 7) & 31) != 0:
            return "jump"                         # c.jr / c.jalr
        return "alu"
    op = insn & 0x7F
    if op == 0x03:
        return "load"
    if op == 0x23:
        return "store"
    if op == 0x63:
        return "branch"
    if op in (0x6F, 0x67):
        return "jump"
    if op == 0x33 and (insn >> 25) == 1:
        return "div" if (insn >> 12) & 7 >= 4 else "mul"
    return "alu"


def run(elf_path, task, iq):
    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    uc.mem_map(0x40000000, 16 * 1024 * 1024)
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                uc.mem_write(seg["p_paddr"], seg.data())
        sym = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        entry = elf["e_entry"]
    uc.mem_write(sym["g_task"], int(task).to_bytes(4, "little"))
    uc.mem_write(sym["g_iq"], iq.astype("<u4").tobytes())
    st = {"on": False, "done": False, "prev": None, "counts": {}, "taken": 0}

    def on_write(uc_, access, addr, size, value, _):
        if addr == sym["g_mark"]:
            if value == 1:
                st["on"] = True
            elif value == 2:
                st["on"] = False
            elif value == 3:
                uc_.emu_stop()

    def on_code(uc_, addr, size, _):
        if not st["on"]:
            st["prev"] = None
            return
        prev = st["prev"]
        if prev and prev[2] in ("branch", "jump") and addr != prev[0] + prev[1]:
            st["taken"] += 1
        raw = int.from_bytes(uc_.mem_read(addr, size), "little")
        kind = classify(raw, size)
        st["counts"][kind] = st["counts"].get(kind, 0) + 1
        st["prev"] = (addr, size, kind)

    uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=sym["g_mark"], end=sym["g_mark"] + 3)
    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.emu_start(entry, 0)
    return st["counts"], st["taken"]


def cycles(counts, taken, model):
    n = sum(counts.values())
    if model == "fast":       # 1 cycle each, +1 per taken branch/jump
        return n + taken
    # slow: +2 per taken branch/jump, +1 per load (load-use), mul 2, div 33
    return (n + 2 * taken + counts.get("load", 0) + counts.get("mul", 0)
            + 32 * counts.get("div", 0))


def main():
    w, _ = cd.capture(5780, [(5781.3, 0.25), (5760, 0.05)])
    res = {}
    for v in ("ref", "new"):
        for task in (1, 2):
            c, t = run(HERE / f"bench_{v}.elf", task, w)
            res[(v, task)] = (c, t)
            n = sum(c.values())
            print(f"{v:3s} task{task}: {n:9d} instr  taken {t:7d}  "
                  + "  ".join(f"{k} {c[k]}" for k in sorted(c)))

    def ms(v, task, model):
        c, t = res[(v, task)]
        return cycles(c, t, model) / F_CPU * 1e3

    print("\nper hop (PSD of 8192 samples + stitch), ms at 240 MHz:")
    for v in ("ref", "new"):
        print(f"  {v}: {ms(v, 1, 'fast'):.1f} .. {ms(v, 1, 'slow'):.1f}")
    ui_cpu = (ms("new", 2, "fast"), ms("new", 2, "slow"))
    print(f"UI processing of one 4CH sweep (10-sweep mean, floor, 4 channels): "
          f"{ui_cpu[0]:.1f} .. {ui_cpu[1]:.1f} ms")

    # Screen: header + spectrum + waterfall + readouts pushed every frame,
    # 320 x 230 px x 16 bit at 40 MHz SPI; LovyanGFX waits on the bus, so the
    # CPU is busy for the transfer, plus ~4 ms of sprite drawing.
    push_ms = 320 * 230 * 16 / 40e6 * 1e3
    draw_ms = push_ms + 4.0
    print(f"screen update: SPI {push_ms:.1f} ms + drawing ~4 ms = {draw_ms:.1f} ms per frame")

    modes = [("4CH", 9), ("3CH", 7), ("1CH", 4)]
    capture_ms = 8192 / 80e6 * 1e3 + 0.1
    for retune in (0.0, 3.0, 6.0):
        print(f"\nretune {retune:.0f} ms per hop (capture {capture_ms:.2f} ms):")
        print("  mode | PC (USB)     | LCD old FFT          | LCD new FFT")
        for name, hops in modes:
            pc = 1000.0 / (hops * (31.0 + retune))
            row = [f"  {name}  | {pc:4.1f} sw/s    "]
            for v in ("ref", "new"):
                rates = []
                for model, ui in (("fast", 0), ("slow", 1)):
                    hop = ms(v, 1, model) + capture_ms + retune
                    sweep = hops * hop
                    proc = ui_cpu[ui]
                    # every sweep is averaged/measured; the screen is drawn
                    # once per sweep, but at most 10 times a second
                    s_all = 1000.0 / (sweep + proc + draw_ms)
                    if s_all > 10.0:
                        s_all = (1000.0 - 10.0 * draw_ms) / (sweep + proc)
                    rates.append(s_all)
                fps = [min(r, 10.0) for r in rates]
                row.append(f"| {min(rates):4.1f}-{max(rates):4.1f} sw/s "
                           f"{min(fps):4.1f}-{max(fps):4.1f} fps ")
            print("".join(row))


if __name__ == "__main__":
    main()
