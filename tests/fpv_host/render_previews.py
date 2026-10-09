"""Render the three monitor screens from synthetic captures (firmware DSP and
drawing code, run on the PC) into PNG files.

    python3 render_previews.py /path/to/fpv-band-monitor-c5 [out_dir]

Both builds: screen_*.png (2.8" ILI9341, 320 x 240) and screen35_*.png
(3.5" Pico-ResTouch-LCD-3.5, 480 x 320).
"""
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.argv += [] if len(sys.argv) > 1 else ["/tmp/fpv-band-monitor-c5"]
import check_dsp as cd  # noqa: E402  (shares the capture model)

HERE = Path(__file__).resolve().parent
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "previews"

SCENES = [
    # mode, file, channels, carriers (MHz, amplitude)
    (0, "4ch", ["E2", "E1", "F3", "F5"], [(5685.4, 0.20), (5704.2, 0.12), (5780.0, 0.30)]),
    (1, "3ch", ["E1", "F1", "F4"], [(5705.6, 0.15), (5739.1, 0.25), (5801.0, 0.08)]),
    (2, "1ch", ["F4"], [(5800.7, 0.20)]),
]


def main():
    subprocess.run(["make", "-s", "-C", str(HERE), "host_dsp", "host_ui", "host_ui35"], check=True)
    OUT.mkdir(exist_ok=True)
    for mode, name, chans, carriers in SCENES:
        freqs = [cd.parse_channels(c)[0].freq_mhz for c in chans]
        start, stop = min(freqs) - 15.0, max(freqs) + 15.0
        st = cd.Stitcher(start, stop, cd.EspSettings(step_mhz=25, usable_mhz=28.0, dc_khz=500.0), 78125)
        words = [cd.capture(lo, carriers)[0] for lo in st.los]
        (HERE / "iq.bin").write_bytes(np.concatenate(words).tobytes())
        cd.run("sweep", "iq.bin", cd.N, start, stop, 25, 28.0, 500.0, *[int(f) for f in freqs])
        for exe, tag in (("host_ui", "screen"), ("host_ui35", "screen35")):
            ppm = HERE / f"{name}.ppm"
            r = subprocess.run([str(HERE / exe), str(mode), "sweep.bin", str(start), str(stop),
                                str(ppm), "1" if mode == 2 else "0", *chans],
                               check=True, capture_output=True, text=True, cwd=HERE)
            print(r.stdout.strip())
            img = Image.open(ppm)
            img.save(OUT / f"{tag}_{name}.png")
            img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(OUT / f"{tag}_{name}_x2.png")
            ppm.unlink()
            if mode == 0:                      # setup screen too
                ppm = HERE / "setup.ppm"
                subprocess.run([str(HERE / exe), str(mode), "sweep.bin", str(start), str(stop),
                                str(ppm), "0", *chans], check=True, capture_output=True, cwd=HERE,
                               env={"FPV_SETUP": "0"})
                img = Image.open(ppm)
                img.save(OUT / f"{tag}_setup.png")
                img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(OUT / f"{tag}_setup_x2.png")
                ppm.unlink()


if __name__ == "__main__":
    main()
