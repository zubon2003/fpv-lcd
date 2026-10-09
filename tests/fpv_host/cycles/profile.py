"""Per-function instruction counts of bench tasks (where the time goes)."""
import sys
from pathlib import Path
from collections import Counter
from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_RISCV, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_RISCV32, Uc
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent)); sys.argv += [] if len(sys.argv) > 1 else ["/tmp/fpv-band-monitor-c5"]
import check_dsp as cd

def prof(elf_path, task, iq):
    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32); uc.mem_map(0x40000000, 16 << 20)
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD": uc.mem_write(seg["p_paddr"], seg.data())
        syms = [s for s in elf.get_section_by_name(".symtab").iter_symbols()]
        sym = {s.name: s["st_value"] for s in syms}
        funcs = sorted((s["st_value"], s["st_value"] + s["st_size"], s.name) for s in syms
                       if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0)
        entry = elf["e_entry"]
    uc.mem_write(sym["g_task"], int(task).to_bytes(4, "little"))
    uc.mem_write(sym["g_iq"], iq.astype("<u4").tobytes())
    st = {"on": False}; cnt = Counter()
    import bisect
    starts = [f[0] for f in funcs]
    def on_write(u, a, ad, sz, v, _):
        if v == 1: st["on"] = True
        elif v == 2: st["on"] = False
        elif v == 3: u.emu_stop()
    def on_code(u, addr, size, _):
        if st["on"]:
            i = bisect.bisect_right(starts, addr) - 1
            cnt[funcs[i][2] if i >= 0 and addr < funcs[i][1] else "?"] += 1
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=sym["g_mark"], end=sym["g_mark"] + 3)
    uc.hook_add(UC_HOOK_CODE, on_code); uc.emu_start(entry, 0)
    return cnt

w, _ = cd.capture(5780, [(5781.3, 0.25)])
for task in (1, 2):
    c = prof(HERE / "bench_new.elf", task, w); tot = sum(c.values())
    print(f"task{task} total {tot}")
    for k, v in c.most_common(12): print(f"  {v:9d} {100*v/tot:5.1f}%  {k}")
