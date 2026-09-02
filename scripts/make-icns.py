"""Pack the logo into a macOS .icns for the .app bundle.

Renders assets/logo-mark-macos.svg -- the variant drawn on Apple's icon grid,
with the margin and squircle edge -- into the icon types the Finder, the Dock and
Spotlight ask for.  Every entry is a PNG payload, which macOS 10.7 and later
read directly -- no iconutil (and so no macOS) needed to build it.

Needs rsvg-convert.  Run from anywhere:

    python3 scripts/make-icns.py
"""
import os
import struct
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = f"{ROOT}/assets/logo-mark-macos.svg"
OUT = f"{ROOT}/assets/transcriptor.icns"

# OSType -> pixel size.  The pairs are the 1x and 2x forms of the same slot:
# ic11 is 16pt@2x, ic12 is 32pt@2x, and so on.
TYPES = [
    (b"icp4", 16),
    (b"icp5", 32),
    (b"ic11", 32),    # 16pt @2x
    (b"ic07", 128),
    (b"ic12", 64),    # 32pt @2x
    (b"ic08", 256),
    (b"ic13", 256),   # 128pt @2x
    (b"ic09", 512),
    (b"ic14", 512),   # 256pt @2x
    (b"ic10", 1024),  # 512pt @2x
]


def main():
    entries = []
    with tempfile.TemporaryDirectory() as tmp:
        rendered = {}
        for ostype, size in TYPES:
            if size not in rendered:
                png = f"{tmp}/{size}.png"
                subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size),
                                SRC, "-o", png], check=True)
                with open(png, "rb") as fh:
                    rendered[size] = fh.read()
            entries.append(ostype + struct.pack(">I", 8 + len(rendered[size]))
                           + rendered[size])

    body = b"".join(entries)
    with open(OUT, "wb") as fh:
        fh.write(b"icns" + struct.pack(">I", 8 + len(body)) + body)
    print(f"{OUT}  {8 + len(body)} bytes, {len(entries)} images")


if __name__ == "__main__":
    main()
