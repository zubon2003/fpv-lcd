"""Compare the firmware DSP (main/fpv, built as ./host_dsp) with the Python
fpv-band-monitor-c5 implementation on synthetic ESP32-C5 captures.

    python3 check_dsp.py /path/to/fpv-band-monitor-c5

Synthetic hop: FM-ATV-like carriers (FM by low-passed noise, ~3 MHz peak
deviation), receiver noise, a DC offset and an I/Q-imbalance image, quantised
to IQ10 words exactly as the C5 delivers them (complex = I - jQ).
"""
import subprocess
import sys
from pathlib import Path

import numpy as np

REF = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/fpv-band-monitor-c5")
sys.path.insert(0, str(REF))
from fpv_band_monitor.analysis import SpectrumAverager, auto_search_halfwidth, measure_all  # noqa: E402
from fpv_band_monitor.channels import parse_channels  # noqa: E402
from fpv_band_monitor.esp_sdr import EspSettings, Stitcher, hop_spectrum_db  # noqa: E402
from fpv_band_monitor.sweep import FrequencyGrid  # noqa: E402

HERE = Path(__file__).resolve().parent
EXE = HERE / "host_dsp"
FS = 80e6
N = 8192
rng = np.random.default_rng(7)


def fm_carrier(offset_hz, amp, dev_hz=3e6):
    t = np.arange(N) / FS
    m = rng.standard_normal(N + 64)
    k = np.ones(64) / 64.0                     # ~1 MHz low-pass "video"
    m = np.convolve(m, k, mode="valid")[:N]
    m = m / (np.max(np.abs(m)) + 1e-9)
    phase = 2 * np.pi * np.cumsum(dev_hz * m) / FS
    return amp * np.exp(1j * (2 * np.pi * offset_hz * t + phase))


def capture(lo_mhz, carriers, noise=0.004, dc=0.01 + 0.006j, image=0.03):
    x = np.zeros(N, complex)
    for f_mhz, amp in carriers:
        off = (f_mhz - lo_mhz) * 1e6
        if abs(off) < 45e6:
            x += fm_carrier(off, amp)
    x += noise * (rng.standard_normal(N) + 1j * rng.standard_normal(N))
    x = x + image * np.conj(x) + dc          # receiver artefacts at fixed LO offsets
    i = np.clip(np.round(x.real * 512), -512, 511).astype(np.int64)
    q = np.clip(np.round(-x.imag * 512), -512, 511).astype(np.int64)
    words = (i & 1023) | ((q & 1023) << 10)
    ref = (i - 1j * q).astype(np.complex64) / 512.0   # what unpack_iq returns
    return words.astype(np.uint32), ref


def run(*args):
    return subprocess.run([str(EXE), *map(str, args)], check=True,
                          capture_output=True, cwd=HERE).stdout


def check_psd():
    w, ref = capture(5780, [(5781.3, 0.25), (5760, 0.05)])
    (HERE / "iq.bin").write_bytes(w.tobytes())
    c = np.frombuffer(run("psd", "iq.bin", N), np.float32)
    py = hop_spectrum_db(ref, 1024)
    cdb = 10 * np.log10(np.maximum(c, 1e-15))
    keep = np.ones(1024, bool)
    keep[510:515] = False                     # DC bin (mean removal rounding)
    d = np.abs(cdb - py)[keep]
    print(f"hop PSD: max |C - Python| = {d.max():.3f} dB, mean {d.mean():.4f} dB "
          f"(over {keep.sum()} bins, signal peak {py.max():.1f} dB, floor {np.median(py):.1f} dB)")
    return d.max() < 0.5 and d.mean() < 0.05


def check_sweep(name, chans, carriers, margin=15.0):
    channels = parse_channels(",".join(chans))
    freqs = [c.freq_mhz for c in channels]
    start, stop = min(freqs) - margin, max(freqs) + margin
    es = EspSettings(step_mhz=25, usable_mhz=28.0, dc_khz=500.0)
    st = Stitcher(start, stop, es, 78125)
    words, spectra = [], []
    for lo in st.los:
        w, ref = capture(lo, carriers)
        words.append(w)
        spectra.append(hop_spectrum_db(ref, st.nfft))
    (HERE / "iq.bin").write_bytes(np.concatenate(words).tobytes())
    py = st.combine(spectra)
    out = run("sweep", "iq.bin", N, start, stop, 25, 28.0, 500.0, *[int(f) for f in freqs]).decode()
    lines = out.strip().splitlines()
    print(f"\n[{name}] {lines[0]}   (Python: {len(st.los)} hops {st.los[0]}..{st.los[-1]}, {st.n} bins)")
    c = np.frombuffer((HERE / "sweep.bin").read_bytes(), np.float32)
    cdb = np.where(np.isfinite(c), 10 * np.log10(np.maximum(c, 1e-15)), np.nan)
    both = np.isfinite(cdb) & np.isfinite(py)
    d = np.abs(cdb - py)[both]
    gaps = int(np.sum(np.isfinite(cdb) != np.isfinite(py)))
    print(f"  stitched sweep: max |C - Python| = {d.max():.3f} dB over {both.sum()} bins, "
          f"coverage mismatches {gaps}")

    grid = FrequencyGrid(st.hz_low / 1e6, st.hz_low / 1e6 + st.n * st.bin_hz / 1e6, st.bin_hz)
    ms = measure_all(py.astype(np.float32), grid, channels,
                     search_mhz=auto_search_halfwidth(channels, 8.0))
    ok = d.max() < 0.5 and gaps == 0
    truth = {round(f): f for f, _ in carriers}
    for m, line in zip(ms, lines[1:]):
        f = line.split()
        cc, cb3, cb20, cpk, csnr, cfl, cpr = map(float, f[2:])
        tx = [t for t in truth.values() if abs(t - m.nominal_mhz) < 8]
        tx_s = f"{tx[0]:.2f}" if tx else "  --  "
        pc = m.centre_mhz if m.centre_mhz is not None else float("nan")
        print(f"  {m.name}: TX {tx_s}  C {cc:8.3f}  Py {pc:8.3f}  "
              f"bw3 C {cb3:5.2f} Py {m.bw_db3_mhz or float('nan'):5.2f}  "
              f"SNR C {csnr:5.1f} Py {m.snr_db:5.1f}  present C {int(cpr)} Py {int(m.present)}")
        if bool(cpr) != m.present:
            ok = False
        if m.present and abs(cc - pc) > 0.05:
            ok = False
    return ok


def check_mean(nframes=10, nbins=2112):
    """10 stored sweeps averaged = PC SpectrumAverager("mean") over them."""
    lin = 10 ** (rng.uniform(-9, -2, (nframes, nbins)) )
    lin[rng.random((nframes, nbins)) < 0.05] = np.inf      # bins a sweep missed
    lin[:, :7] = np.inf                                    # never covered
    (HERE / "iq.bin").write_bytes(lin.astype(np.float32).tobytes())
    c = np.frombuffer(run("mean", "iq.bin", nframes, nbins), np.float32)
    av = SpectrumAverager(window_s=100.0)
    for t, frame in enumerate(lin):
        db = np.where(np.isfinite(frame), 10 * np.log10(frame), np.nan).astype(np.float32)
        py = av.add(float(t), db)
    cdb = np.where(np.isfinite(c), 10 * np.log10(c), np.nan)
    both = np.isfinite(cdb) & np.isfinite(py)
    d = np.abs(cdb - py)[both]
    mism = int(np.sum(np.isfinite(cdb) != np.isfinite(py)))
    print(f"\n10-sweep mean: max |C - Python| = {d.max():.4f} dB over {both.sum()} bins, "
          f"gap mismatches {mism}")
    return d.max() < 0.01 and mism == 0


def main():
    subprocess.run(["make", "-s", "-C", str(HERE)], check=True)
    results = [check_psd(), check_mean()]
    results.append(check_sweep("4CH", ["E2", "E1", "F3", "F5"],
                               [(5685.4, 0.20), (5704.2, 0.12), (5780.0, 0.30)]))
    results.append(check_sweep("3CH", ["E1", "F1", "F4"],
                               [(5705.6, 0.15), (5739.1, 0.25), (5801.0, 0.08)]))
    results.append(check_sweep("1CH F3", ["F3"], [(5780.7, 0.20)]))
    print("\nPASS" if all(results) else "\nFAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
