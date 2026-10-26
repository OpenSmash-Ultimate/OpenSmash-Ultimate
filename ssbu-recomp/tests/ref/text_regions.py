"""Font rasteriser check (audit row "fonts: rasteriser"): compare TEXT regions of the port's checkpoint frames with the
reference emulator's golden frames.

    python tests/ref/text_regions.py [--port <dir>] [--crops <dir>]       (default port dir: ref-emu/work/port)

Boxes are static text of the checkpoints (hand-placed on the golden frames, 1920x1080 picture coordinates); animated or
selected items are left out. Per box, on luminance:
  mad   mean absolute difference, 0..255 (0 = identical)
  ncc   zero-mean normalised cross-correlation, best over shifts of -2..2 pixels in x and y (1 = same shapes)
  shift the shift (dx, dy) of the port that gives that best value (0,0 = glyphs at the same pixels)
  ink   intersection / union of the "ink" pixels (each side thresholded half-way between its own darkest and lightest
        5 % of the box), at the best shift (1 = same glyph coverage)
  width difference of the summed ink width in pixels (port - reference): emboldening / thinning shows here
The reference is an emulator at the console's 1080p output: it rasterises with the game's own font engine code path of
the system module as the EMULATOR implements it, not a console capture."""
import os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refcfg

S = 1.5          # boxes below were read off 1280x720 reductions
BOXES = {
    'rules': [('title', 95, 12, 380, 40), ('recent rule sets', 270, 108, 245, 36), ('create rule set', 232, 205, 380, 48),
              ('list entry time', 315, 408, 260, 46), ('rule style', 930, 118, 120, 48), ('stock label', 890, 216, 70, 36), ('stock value', 1040, 214, 30, 40),
              ('time label', 878, 292, 105, 38), ('time value', 1048, 290, 90, 40), ('spirits label', 890, 368, 95, 38), ('spirits value', 1055, 366, 160, 40),
              ('fifth setting', 872, 444, 145, 38), ('edit button', 815, 634, 100, 28)],
    'stage_select': [('rules header', 215, 8, 280, 38), ('stage form', 828, 14, 82, 36)],
}


def lum(path):
    a = np.asarray(Image.open(path).convert('RGB').resize((1920, 1080)), dtype=np.float32)
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def ink(a):
    lo, hi = np.percentile(a, 5), np.percentile(a, 95)
    if hi - lo < 24: return np.zeros(a.shape, bool), 0
    m = a > (lo + hi) / 2
    if m.mean() > 0.5: m = ~m           # the ink is the minority side
    return m, 1


def ncc(a, b):
    a, b = a - a.mean(), b - b.mean()
    d = float(np.sqrt((a * a).sum() * (b * b).sum()))
    return float((a * b).sum() / d) if d > 0 else 0.0


def main():
    args = sys.argv[1:]
    port = args[args.index('--port') + 1] if '--port' in args else os.path.join(refcfg.ROOT, 'ref-emu', 'work', 'port')
    crops = args[args.index('--crops') + 1] if '--crops' in args else None
    if crops: os.makedirs(crops, exist_ok=True)
    print('| checkpoint | text | mad | ncc | shift | ink IoU | ink pixels ref / port |')
    print('|---|---|---:|---:|---|---:|---|')
    allv = []
    for name, boxes in BOXES.items():
        rp, pp = os.path.join(refcfg.GOLDEN, name + '.png'), os.path.join(port, name + '.png')
        if not (os.path.exists(rp) and os.path.exists(pp)): print('| %s | (frame missing) | | | | | |' % name); continue
        R, P = lum(rp), lum(pp)
        for label, x, y, w, h in boxes:
            x, y, w, h = int(x * S), int(y * S), int(w * S), int(h * S)
            r = R[y:y + h, x:x + w]
            best = (-2.0, 0, 0)
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    v = ncc(r, P[y + dy:y + dy + h, x + dx:x + dx + w])
                    if v > best[0]: best = (v, dx, dy)
            v, dx, dy = best
            p0 = P[y:y + h, x:x + w]
            p = P[y + dy:y + dy + h, x + dx:x + dx + w]
            mr, okr = ink(r); mp, okp = ink(p)
            iou = float((mr & mp).sum()) / max(1, int((mr | mp).sum())) if okr and okp else float('nan')
            mad = float(np.abs(r - p0).mean())
            allv.append((mad, v, iou))
            print('| %s | %s | %.1f | %.3f | %d,%d | %.3f | %d / %d |' % (name, label, mad, v, dx, dy, iou, int(mr.sum()), int(mp.sum())))
            if crops:
                both = np.concatenate([r, np.full((h, 6), 255, np.float32), p0], axis=1)
                Image.fromarray(np.clip(both, 0, 255).astype(np.uint8)).save(os.path.join(crops, '%s_%s.png' % (name, ''.join(c if c.isalnum() else '_' for c in label))))
    if allv:
        a = np.array(allv, dtype=np.float64)
        print()
        print('%d text boxes: mad mean %.1f max %.1f; ncc mean %.3f min %.3f; ink IoU mean %.3f min %.3f' % (len(allv), a[:, 0].mean(), a[:, 0].max(), a[:, 1].mean(), a[:, 1].min(), np.nanmean(a[:, 2]), np.nanmin(a[:, 2])))


if __name__ == '__main__':
    main()
