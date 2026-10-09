"""End-to-end check of the USB link: firmware encoder (C, via host_dsp) ->
pseudo-terminal -> the PC monitor's fpv_link receiver -> its display grid
and measurement.

    python3 check_link.py /path/to/fpv-band-monitor-c5   (the PC version, with fpv_link.py)

The fake device answers like the firmware's link.c / monitor.cpp: FPV?, FPV
SPAN, FPV GAIN, FPV STREAM, and streams SWEEP frames whose payload is what
fpv_encode() produces for a stitched sweep.
"""
import os
import subprocess
import sys
import threading
import time
import tty
import zlib
from pathlib import Path

import numpy as np

PC = Path(sys.argv[1] if len(sys.argv) > 1 else "/home/claude/work/fpv-band-monitor-c5")
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.argv = [sys.argv[0], str(PC)]       # check_dsp imports the PC version from here too
import check_dsp as cd  # noqa: E402

sys.path.insert(0, str(PC))
from fpv_band_monitor import fpv_link  # noqa: E402
from fpv_band_monitor.analysis import measure_all  # noqa: E402
from fpv_band_monitor.channels import parse_channels  # noqa: E402
from fpv_band_monitor.cli import _span_planner, build_parser  # noqa: E402
from fpv_band_monitor.esp_sdr import EspSettings  # noqa: E402
from fpv_band_monitor.sweep import DeviceInfo, SweepAccumulator, SweepSettings  # noqa: E402


class FakeFirmware(threading.Thread):
    def __init__(self, fd, frames, stock=False):
        super().__init__(daemon=True)
        self.fd, self.frames, self.stock = fd, frames, stock
        self.stream = False
        self.span = None
        self.gain = None
        self.log = []
        self.stop = threading.Event()

    def send(self, b):
        os.write(self.fd, b)

    def run(self):
        buf = b""
        seq = 0
        next_t = time.monotonic()
        os.set_blocking(self.fd, False)
        while not self.stop.is_set():
            try:
                buf += os.read(self.fd, 4096)
            except BlockingIOError:
                pass
            except OSError:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode().strip()
                if not text:
                    continue                      # firmware ignores blank lines
                self.log.append(text)
                if self.stock or not text.upper().startswith("FPV"):
                    self.send(b"ERR command\n")
                elif text == "FPV?":
                    self.send(b"FPV 1 bin_hz=78125 maxbins=2304 step=25 usable=28 dc_khz=500 samples=8192\n")
                elif text.startswith("FPV SPAN"):
                    self.span = text
                    self.send(b"OK\n")
                elif text.startswith("FPV GAIN"):
                    self.gain = text
                    self.send(b"OK\n")
                elif text == "FPV STREAM ON":
                    self.stream = True
                    self.send(b"OK\n")
                elif text == "FPV STREAM OFF":
                    self.stream = False
                    self.send(b"OK\n")
                else:
                    self.send(b"ERR unknown FPV command\n")
            if self.stream and time.monotonic() >= next_t and not self.stock:
                start, nbins, payload = self.frames[seq % len(self.frames)]
                seq += 1
                crc = zlib.crc32(payload)
                self.send(f"SWEEP {seq} {start} {nbins} 78125 {seq * 100} {crc:08x}\n".encode() + payload)
                next_t = time.monotonic() + 0.1
            time.sleep(0.002)


def pty_pair():
    master, slave = os.openpty()
    tty.setraw(slave)
    return master, os.ttyname(slave), slave


def main():
    subprocess.run(["make", "-s", "-C", str(HERE), "host_dsp"], check=True)
    chans = ["E2", "E1", "F3", "F5"]
    channels = parse_channels(",".join(chans))
    start, stop = 5670.0, 5835.0
    carriers = [(5685.4, 0.20), (5704.2, 0.12), (5780.0, 0.30)]
    st = cd.Stitcher(start, stop, cd.EspSettings(step_mhz=25, usable_mhz=28.0, dc_khz=500.0), 78125)
    words = [cd.capture(lo, carriers)[0] for lo in st.los]
    (HERE / "iq.bin").write_bytes(np.concatenate(words).tobytes())
    cd.run("sweep", "iq.bin", cd.N, start, stop, 25, 28.0, 500.0, *[int(c.freq_mhz) for c in channels])
    sweep = np.frombuffer((HERE / "sweep.bin").read_bytes(), np.float32)
    payload = subprocess.run([str(HERE / "host_dsp"), "encode", "sweep.bin", str(sweep.size)],
                             check=True, capture_output=True, cwd=HERE).stdout
    frames = [(5670, sweep.size, payload)]
    ok = True

    # 1. detection: LCD firmware yes, stock ESP-SDR no
    for stock in (False, True):
        m, path, s = pty_pair()
        dev = FakeFirmware(m, frames, stock=stock)
        dev.start()
        found = fpv_link.detect_firmware(path, timeout_s=0.5)
        dev.stop.set()
        os.close(s)
        print(f"detect_firmware on {'stock ESP-SDR' if stock else 'LCD firmware'}: {found}")
        ok = ok and found == (not stock)

    # 2. stream through the PC receiver and onto its display grid
    m, path, s = pty_pair()
    dev = FakeFirmware(m, frames)
    dev.start()
    args = build_parser().parse_args(["--channels", ",".join(chans)])
    args.backend, args.bin_khz, args.step = "fw", 78.125, 25
    grid, _los = _span_planner(args)(start, stop)
    es = EspSettings(backend="fw", channels=tuple((c.name, c.freq_mhz) for c in channels))
    settings = SweepSettings(start_mhz=start, stop_mhz=stop, bin_hz=78125, gain=40, port=path)
    stop_ev = threading.Event()
    got = []
    t0 = time.monotonic()
    for item in fpv_link.fw_source(es)(settings, stop_ev):
        if isinstance(item, DeviceInfo):
            print(f"connected: {item.identity}")
            continue
        got.append(item)
        if len(got) == 5 or time.monotonic() - t0 > 5:
            stop_ev.set()
            break
    dev.stop.set()
    print(f"device saw: {dev.span!r}, {dev.gain!r}; frames received: {len(got)}")
    ok = ok and dev.span == "FPV SPAN 5670.000 5835.000 E2,E1,F3,F5" and dev.gain == "FPV GAIN 40" and len(got) == 5

    line = got[0]
    acc = SweepAccumulator(grid)
    acc.paint(line.hz_low, line.bin_width, line.powers)
    shown = acc.powers
    ref = np.where(np.isfinite(sweep), 10 * np.log10(np.maximum(sweep, 1e-30)), np.nan)
    both = np.isfinite(shown) & np.isfinite(ref)
    err = float(np.max(np.abs(shown[both] - ref[both])))
    print(f"grid {grid.n} bins from {grid.start_mhz:.3f} MHz (firmware {sweep.size} from 5670); "
          f"received vs firmware sweep: max {err:.4f} dB over {both.sum()} bins")
    ok = ok and grid.n == sweep.size and abs(grid.start_mhz - 5670.0) < 1e-9 and err < 0.006

    ms_rx = measure_all(shown.astype(np.float32), grid, channels, search_mhz=8.0)
    ms_c = measure_all(ref.astype(np.float32), grid, channels, search_mhz=8.0)
    for a, b in zip(ms_rx, ms_c):
        ca = a.centre_mhz if a.centre_mhz is not None else float("nan")
        cb = b.centre_mhz if b.centre_mhz is not None else float("nan")
        print(f"  {a.name}: centre via USB {ca:9.4f}  direct {cb:9.4f}  present {a.present}/{b.present}")
        ok = ok and a.present == b.present and (not a.present or abs(ca - cb) < 0.002)
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
