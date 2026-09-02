"""Regenerate the Transcriptor logo SVGs from the UI's own display font.

Needs `fonttools` and `brotli`; the glyphs are emitted as outlines, so the
results carry no font dependency.  logo-mark.svg is what the Windows icon is
built from -- rerun scripts/make-icon.py after changing anything here.  PNG
exports:

    for s in 256 1024; do
      rsvg-convert -w $s -h $s assets/logo-mark.svg -o assets/logo-$s.png
    done
"""
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.boundsPen import BoundsPen

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = f"{ROOT}/web/fonts/bricolage-grotesque-800-normal-latin.woff2"
OUT = f"{ROOT}/assets"

INK = "#f4efe4"
REC = "#ff5b2e"
BG = "#000000"

f = TTFont(FONT)
gs = f.getGlyphSet()
cmap = f.getBestCmap()
UPEM = f["head"].unitsPerEm
TRACK = -0.03 * UPEM


def run(text):
    """Return (glyphs, bbox) in font units, baseline at y=0, y up."""
    x = 0.0
    out = []
    x0 = y0 = 1e9
    x1 = y1 = -1e9
    for ch in text:
        g = cmap[ord(ch)]
        pen = SVGPathPen(gs)
        gs[g].draw(pen)
        bp = BoundsPen(gs)
        gs[g].draw(bp)
        if bp.bounds:
            bx0, by0, bx1, by1 = bp.bounds
            x0 = min(x0, x + bx0); x1 = max(x1, x + bx1)
            y0 = min(y0, by0);     y1 = max(y1, by1)
        out.append((ch, x, pen.getCommands()))
        x += gs[g].width + TRACK
    return out, (x0, y0, x1, y1)


def paths(text, colors, scale, tx, ty, indent="  "):
    """SVG <path> elements; colors maps index->fill. y flipped."""
    glyphs, _ = run(text)
    s = []
    for i, (ch, x, d) in enumerate(glyphs):
        fill = colors(i, ch)
        s.append(
            f'{indent}<path fill="{fill}" transform="translate({tx + x * scale:.2f} {ty:.2f}) '
            f'scale({scale:.5f} {-scale:.5f})" d="{d}"/>'
        )
    return "\n".join(s)


def fit(text, box_w, box_h, cx, cy):
    """Scale + baseline placement so the run's ink bbox is centred in box."""
    _, (x0, y0, x1, y1) = run(text)
    w, h = x1 - x0, y1 - y0
    scale = min(box_w / w, box_h / h)
    tx = cx - (x0 + w / 2) * scale
    ty = cy + (y0 + h / 2) * scale   # baseline y in flipped space
    return scale, tx, ty


def mark(size=512, bg=True, radius_ratio=0.2237, pad=0.155, plate=BG, t=INK):
    box = size * (1 - 2 * pad)
    scale, tx, ty = fit("tor", box, box, size / 2, size / 2)
    color = lambda i, ch: t if ch == "t" else REC
    body = paths("tor", color, scale, tx, ty)
    r = size * radius_ratio
    plate = (f'  <rect width="{size}" height="{size}" rx="{r:.2f}" ry="{r:.2f}" fill="{plate}"/>\n'
             if bg else "")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" '
            f'width="{size}" height="{size}" role="img" aria-label="Transcriptor">\n'
            f'  <title>Transcriptor</title>\n{plate}{body}\n</svg>\n')


def squircle(cx, cy, half, n=5.0, steps=512):
    """Superellipse |x|^n + |y|^n = 1 -- the continuous corner macOS uses.

    A circular-arc rounded rect visibly differs from the system icons next to
    it; n=5 tracks Apple's shape closely enough at every size we ship.
    """
    import math
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = math.copysign(abs(c) ** (2 / n), c)
        y = math.copysign(abs(s) ** (2 / n), s)
        pts.append(f"{cx + half * x:.2f} {cy + half * y:.2f}")
    return "M" + "L".join(pts) + "Z"


def mark_macos(size=1024, margin=0.0977, plate=BG, t=INK):
    """Icon on Apple's grid: the body fills 824 of 1024, with a squircle edge.

    The margin is what keeps it from looming over its neighbours in the Dock;
    macOS adds the drop shadow itself, so none is baked in here.
    """
    body_size = size * (1 - 2 * margin)
    box = body_size * 0.69          # same glyph-to-plate ratio as mark()
    scale, tx, ty = fit("tor", box, box, size / 2, size / 2)
    color = lambda i, ch: t if ch == "t" else REC
    glyphs = paths("tor", color, scale, tx, ty)
    shape = squircle(size / 2, size / 2, body_size / 2)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" '
            f'width="{size}" height="{size}" role="img" aria-label="Transcriptor">\n'
            f'  <title>Transcriptor</title>\n'
            f'  <path fill="{plate}" d="{shape}"/>\n{glyphs}\n</svg>\n')


def wordmark(height=160, pad=0.12):
    text = "transcriptor"
    _, (x0, y0, x1, y1) = run(text)
    # scale from cap/ascender height of the run
    box_h = height * (1 - 2 * pad)
    scale = box_h / (y1 - y0)
    tx = height * pad - x0 * scale
    ty = height * pad + y1 * scale
    width = tx + x1 * scale + height * pad
    color = lambda i, ch: REC if i >= len(text) - 2 else INK
    body = paths(text, color, scale, tx, ty)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.0f} {height}" '
            f'width="{width:.0f}" height="{height}" role="img" aria-label="Transcriptor">\n'
            f'  <title>Transcriptor</title>\n{body}\n</svg>\n')


os.makedirs(OUT, exist_ok=True)
open(f"{OUT}/logo-mark.svg", "w").write(mark())
open(f"{OUT}/logo-mark-plain.svg", "w").write(mark(bg=False))
open(f"{OUT}/logo-mark-light.svg", "w").write(mark(plate="#f5f0e6", t="#1b160e"))
open(f"{OUT}/logo-mark-macos.svg", "w").write(mark_macos())
open(f"{OUT}/logo-wordmark.svg", "w").write(wordmark())
print("written")
