"""Pack the logo into a Windows .ico for the executable's resources.

Renders assets/logo-mark.svg at every size Explorer, the taskbar and the
Alt-Tab switcher ask for.  Sizes up to 128 are stored as 32-bit BGRA DIBs
(what every Windows version understands); the 256 entry is stored as PNG,
which is how Vista and later expect the jumbo icon to be carried.

Needs rsvg-convert and ImageMagick.  Run from anywhere:

    python3 scripts/make-icon.py
"""
import os
import struct
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = f"{ROOT}/assets/logo-mark.svg"
OUTS = [f"{ROOT}/src/platform/app.ico", f"{ROOT}/assets/transcriptor.ico"]

DIB_SIZES = [16, 24, 32, 48, 64, 128]
PNG_SIZES = [256]


def render(size, path):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), SRC,
                    "-o", path], check=True)


def dib_entry(size, png):
    """32-bit BGRA DIB: header, bottom-up pixels, then an empty AND mask."""
    raw = subprocess.run(["magick", png, "-depth", "8", "BGRA:-"],
                         check=True, capture_output=True).stdout
    stride = size * 4
    rows = [raw[y * stride:(y + 1) * stride] for y in range(size)]
    pixels = b"".join(reversed(rows))
    mask_stride = ((size + 31) // 32) * 4          # 1 bpp, 4-byte aligned
    mask = b"\0" * (mask_stride * size)
    header = struct.pack("<IiiHHIIiiII",
                         40,            # biSize
                         size,          # biWidth
                         size * 2,      # biHeight: colour + mask
                         1, 32, 0,      # planes, bpp, BI_RGB
                         len(pixels) + len(mask),
                         0, 0, 0, 0)
    return header + pixels + mask


def main():
    entries = []   # (width, height, bpp, payload)
    with tempfile.TemporaryDirectory() as tmp:
        for size in DIB_SIZES:
            png = f"{tmp}/{size}.png"
            render(size, png)
            entries.append((size, size, 32, dib_entry(size, png)))
        for size in PNG_SIZES:
            png = f"{tmp}/{size}.png"
            render(size, png)
            with open(png, "rb") as fh:
                entries.append((size, size, 32, fh.read()))

    offset = 6 + 16 * len(entries)
    directory, blobs = b"", b""
    for w, h, bpp, data in entries:
        directory += struct.pack("<BBBBHHII",
                                 w & 0xFF, h & 0xFF,   # 0 means 256
                                 0, 0,                 # palette, reserved
                                 1, bpp,
                                 len(data), offset)
        blobs += data
        offset += len(data)

    ico = struct.pack("<HHH", 0, 1, len(entries)) + directory + blobs
    for out in OUTS:
        with open(out, "wb") as fh:
            fh.write(ico)
        print(f"{out}  {len(ico)} bytes, {len(entries)} images")


if __name__ == "__main__":
    main()
