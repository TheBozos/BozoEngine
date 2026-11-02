"""The Bozos' logo (python make_logo.py) and Twisted Metal: Black's variant (python make_logo.py tmb): a clown face drawn from retro console shapes (cross and triangle eyes, a round red nose, square teeth),
on the PS2 BIOS blue with boot towers, in RecompWatcher's colours. Writes PNGs: the icon, the wordmark, a banner."""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# Output folder and the Exo 2 font (OFL, Google Fonts: Exo2[wght].ttf): BOZO_LOGO_OUT, BOZO_LOGO_FONT.
OUT = os.environ.get('BOZO_LOGO_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'branding'))
FONT = os.environ.get('BOZO_LOGO_FONT', r'D:\tmb_kit\downloads\exo2-1796e344\Exo2[wght].ttf')
SS = 4  # supersampling

INK = (232, 239, 255)
DIM = (154, 172, 207)
CROSS = (126, 166, 255)
CIRCLE = (255, 107, 122)
TRIANGLE = (79, 224, 163)
SQUARE = (255, 143, 216)
ACCENT = (183, 155, 255)


def font(size, weight):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([weight])
    except Exception:
        pass
    return f


def radial(w, h, cx, cy, r, stops):
    """A radial gradient: stops = [(t, (r,g,b)), ...], t from the centre (0) to r (1)."""
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = min(math.hypot(x - cx, y - cy) / r, 1.0)
            for i in range(len(stops) - 1):
                t0, c0 = stops[i]
                t1, c1 = stops[i + 1]
                if t <= t1:
                    k = (t - t0) / (t1 - t0) if t1 > t0 else 0
                    px[x, y] = tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3))
                    break
    return img


def background(w, h):
    small = radial(w // 8, h // 8, w // 16, h // 8 * 0.92, max(w, h) // 8 * 0.95,
                   [(0, (20, 50, 143)), (0.55, (5, 11, 38)), (1, (1, 3, 10))])
    return small.resize((w, h), Image.BICUBIC)


def towers(size, w, h, base, count, seed=7, dim=1.0):
    """The PS2 boot screen's towers: glowing translucent bars rising from the bottom."""
    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rnd = seed
    bw = w / (count * 1.6)
    for i in range(count):
        rnd = (rnd * 1103515245 + 12345) & 0x7fffffff
        frac = 0.25 + (rnd % 1000) / 1000 * 0.75
        x = (i + 0.3) * w / count
        top = base - frac * size
        alpha = int((40 + (rnd >> 10) % 60) * dim)
        d.rectangle([x, top, x + bw, h], fill=(111, 141, 255, alpha))
        d.rectangle([x, top, x + bw, top + bw * 0.25], fill=(201, 219, 255, int(alpha + 60 * dim)))
    glow = layer.filter(ImageFilter.GaussianBlur(w / 120))
    return Image.alpha_composite(glow, layer)


def glow_shape(canvas, draw_fn, blur, strength=1.0):
    """Draws a shape twice: blurred (its glow) and sharp."""
    g = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(g))
    # Blurred with premultiplied alpha, so the glow keeps the shape's colour (no dark fringe on a light background).
    pre = Image.new('RGBA', g.size)
    r_, g_, b_, a_ = g.split()
    from PIL import ImageChops
    pre = Image.merge('RGBA', [ImageChops.multiply(c, a_) for c in (r_, g_, b_)] + [a_]).filter(ImageFilter.GaussianBlur(blur))
    pr, pg, pb, pa = pre.split()
    import numpy as _np
    arr = _np.asarray(pre).astype(_np.float32)
    alpha = arr[..., 3:4]
    rgb = _np.where(alpha > 0, arr[..., :3] * 255.0 / _np.maximum(alpha, 1), 0)
    halo = Image.fromarray(_np.concatenate([_np.clip(rgb, 0, 255), alpha], axis=2).astype(_np.uint8), 'RGBA')
    if strength != 1.0:
        a = halo.getchannel('A').point(lambda v: min(255, int(v * strength)))
        halo.putalpha(a)
    canvas.alpha_composite(halo)
    canvas.alpha_composite(g)


def clown(canvas, cx, cy, r, fire=False):
    """The clown face, centred at (cx, cy), radius r (in canvas pixels). fire: flames for hair and no hat (Twisted Metal:
    Black's variant)."""
    lw = max(2, int(r * 0.075))
    if fire:
        fire_hair(canvas, cx, cy, r)
    # Hair puffs: two clouds of circles, in the cross and square colours.
    def puffs(d):
        for side, col in ((-1, CROSS), (1, SQUARE)):
            for k, (dx, dy, rr) in enumerate([(1.02, -0.28, 0.36), (1.18, 0.08, 0.34), (0.98, 0.38, 0.30), (0.78, -0.62, 0.28)]):
                x = cx + side * dx * r
                y = cy + dy * r
                d.ellipse([x - rr * r, y - rr * r, x + rr * r, y + rr * r], fill=col + (255,))
    if not fire:
        glow_shape(canvas, puffs, r * 0.12, 0.8)
    # Head.
    def head(d):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=INK + (255,))
    glow_shape(canvas, head, r * 0.08, 0.6)
    d = ImageDraw.Draw(canvas)
    # Small cone hat in the accent violet, a pompom on top.
    hx, hy = cx, cy - r * 0.86
    if fire:
        hx = None
    if hx is not None:
      d.polygon([(hx - r * 0.34, hy + r * 0.1), (hx + r * 0.34, hy + r * 0.1), (hx + r * 0.05, hy - r * 0.78)], fill=ACCENT + (255,))
    if hx is not None:
      d.line([(hx - r * 0.34, hy + r * 0.1), (hx + r * 0.34, hy + r * 0.1)], fill=(120, 90, 210, 255), width=lw)
    if hx is not None:
      d.ellipse([hx + r * 0.05 - r * 0.13, hy - r * 0.78 - r * 0.13, hx + r * 0.05 + r * 0.13, hy - r * 0.78 + r * 0.13], fill=TRIANGLE + (255,))
    # Eyes: a cross (left) and a triangle (right).
    ex, ey, es = r * 0.40, cy - r * 0.27, r * 0.16
    lx = cx - ex
    d.line([(lx - es, ey - es), (lx + es, ey + es)], fill=CROSS + (255,), width=int(lw * 1.5))
    d.line([(lx - es, ey + es), (lx + es, ey - es)], fill=CROSS + (255,), width=int(lw * 1.5))
    rx = cx + ex
    tri = [(rx, ey - es * 1.15), (rx + es * 1.2, ey + es * 0.9), (rx - es * 1.2, ey + es * 0.9)]
    d.polygon(tri, fill=TRIANGLE + (255,))
    k = 0.42  # the inner cut-out: a smaller triangle with the same centre
    gx, gy = rx, (tri[0][1] + tri[1][1] + tri[2][1]) / 3
    d.polygon([(gx + (x - gx) * k, gy + (y - gy) * k) for x, y in tri], fill=INK + (255,))
    # Mouth: a wide grin with square teeth.
    mw, mt, mb = r * 0.6, cy + r * 0.33, cy + r * 0.68
    d.chord([cx - mw, mt - (mb - mt), cx + mw, mb], 0, 180, fill=(40, 22, 60, 255))
    teeth = 4
    tw = mw * 2 / (teeth + 1.6)
    for i in range(teeth):
        x0 = cx - mw + tw * 0.8 + i * tw
        d.rectangle([x0 + tw * 0.08, mt + r * 0.01, x0 + tw * 0.92, mt + r * 0.13], fill=INK + (255,))
    d.arc([cx - mw, mt - (mb - mt), cx + mw, mb], 0, 180, fill=(40, 22, 60, 255), width=lw)
    # Cheeks.
    for side in (-1, 1):
        x, y, rr = cx + side * r * 0.66, cy + r * 0.24, r * 0.12
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=SQUARE + (110,))
    # The nose: the red circle, glowing.
    def nose(dd):
        nr = r * 0.19
        dd.ellipse([cx - nr, cy + r * 0.04 - nr, cx + nr, cy + r * 0.04 + nr], fill=CIRCLE + (255,))
    glow_shape(canvas, nose, r * 0.1, 1.4)
    dd = ImageDraw.Draw(canvas)
    nr = r * 0.19
    dd.ellipse([cx - nr * 0.55, cy + r * 0.04 - nr * 0.62, cx - nr * 0.15, cy + r * 0.04 - nr * 0.22], fill=(255, 220, 225, 200))


def scanlines(img, step):
    over = Image.new('RGBA', img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    for y in range(0, img.size[1], step):
        d.line([(0, y), (img.size[0], y)], fill=(0, 0, 0, 28))
    return Image.alpha_composite(img, over)


def gradient_text(size, text, fnt, c0, c1, tracking):
    """The text with a horizontal gradient and letter spacing, on a transparent layer."""
    w, h = size
    mask = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(mask)
    widths = [d.textlength(ch, font=fnt) for ch in text]
    total = sum(widths) + tracking * (len(text) - 1)
    asc, desc = fnt.getmetrics()
    x = (w - total) / 2
    y = (h - (asc + desc)) / 2
    for ch, cw in zip(text, widths):
        d.text((x, y), ch, font=fnt, fill=255)
        x += cw + tracking
    grad = Image.new('RGBA', (w, h))
    gp = ImageDraw.Draw(grad)
    for xx in range(w):
        k = xx / max(1, w - 1)
        gp.line([(xx, 0), (xx, h)], fill=tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3)) + (255,))
    grad.putalpha(mask)
    return grad, mask


def icon(px):
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.45, W, W, W * 1.02, 9))
    clown(bg, W / 2, W * 0.53, W * 0.26)
    bg = scanlines(bg, SS * 3)
    # Rounded square.
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


def wordmark(w, h, transparent, title='THE BOZOS', subtitle='PS2 RECOMPILATION  ·  BOUBOU666 & ARZAROTH', fire=False, title_size=0.33):
    W, H = w * SS, h * SS
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0)) if transparent else background(W, H).convert('RGBA')
    if not transparent:
        img.alpha_composite(towers(H * 0.55, W, H, H * 1.04, 16, seed=11, dim=0.55))
    cr = H * 0.25
    clown(img, H * 0.62, H * (0.62 if fire else 0.58), cr * (0.92 if fire else 1.0), fire=fire)
    ox, oy = int(H * 1.1), int(H * 0.10)
    tw, th = int(W - ox), int(H * 0.55)
    # The title as large as title_size allows, smaller when it would not fit the space right of the clown.
    size = int(H * title_size)
    while True:
        fnt = font(size, 800)
        probe = ImageDraw.Draw(Image.new('L', (1, 1)))
        tracking = size * 0.075
        width = sum(probe.textlength(ch, font=fnt) for ch in title) + tracking * (len(title) - 1)
        if width <= tw * 0.92 or size <= 8:
            break
        size = int(size * 0.95)
    text, tmask = gradient_text((tw, th), title, fnt, CROSS, ACCENT, tracking)
    # The glow from the letters' shape alone (a blurred mask in the accent colour), so no box shows around them.
    glow = Image.new('RGBA', (tw, th), ACCENT + (0,))
    glow.putalpha(tmask.filter(ImageFilter.GaussianBlur(H * 0.03)).point(lambda v: int(v * 0.7)))
    img.alpha_composite(glow, (ox, oy))
    img.alpha_composite(text, (ox, oy))
    sub = font(int(H * 0.085), 500)
    d = ImageDraw.Draw(img)
    line = subtitle
    lw = d.textlength(line, font=sub)
    bottom = tmask.getbbox()[3] if tmask.getbbox() else th
    d.text((ox + (tw - lw) / 2, oy + bottom + H * 0.07), line, font=sub, fill=DIM + (255,))
    if not transparent:
        img = scanlines(img, SS * 3)
    return img.resize((w, h), Image.LANCZOS)


# ---- per-game variants: the Bozos clown plus a landmark of the game (generic shapes only, no game art) ----

def fire_hair(canvas, cx, cy, r):
    """Flames standing up from the top half of the head, in three layers (outer red to inner yellow), glowing."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    tongues = [(-1.05, 0.55), (-0.85, 0.95), (-0.55, 1.25), (-0.2, 1.45), (0.2, 1.38), (0.55, 1.2), (0.85, 0.92), (1.05, 0.5)]
    for k, col in enumerate([(230, 60, 50), (255, 140, 60), (255, 210, 120)]):
        s = 1.0 - k * 0.24
        for i, (dx, hgt) in enumerate(tongues):
            ang = math.atan2(-1.0, dx * 0.9)
            bx = cx + dx * r * 0.92
            by = cy - math.sqrt(max(0.0, 1 - min(1.0, abs(dx)) ** 2)) * r * 0.55
            tip_x = bx + dx * r * 0.35 * s + (0.08 * r if i % 2 else -0.08 * r) * s
            tip_y = by - hgt * r * s
            w = r * 0.32 * s
            d.polygon([(bx - w, by + r * 0.25), (bx - w * 0.6, by - hgt * r * 0.45 * s), (tip_x, tip_y),
                       (bx + w * 0.6, by - hgt * r * 0.4 * s), (bx + w, by + r * 0.25)], fill=col + (255,))
    glow = layer.filter(ImageFilter.GaussianBlur(r * 0.14))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)


def icon_tmb(px):
    """Twisted Metal: Black: the Bozos clown with its head on fire (car combat), on the team's PS2 blue."""
    W = px * SS
    bg = background(W, W).convert('RGBA')
    bg.alpha_composite(towers(W * 0.4, W, W, W * 1.02, 9))
    clown(bg, W / 2, W * 0.6, W * 0.25, fire=True)
    bg = scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


USAGE = """The Bozos' logos (PNG, rendered with Pillow at 4x and scaled down).
  python make_logo.py                      the team set: bozos_icon_1024/256.png, bozos_banner.png, bozos_wordmark_transparent.png
  python make_logo.py tmb                  Twisted Metal: Black: the clown with its head on fire, tmb_icon_*.png, tmb_banner.png
  python make_logo.py engine               BozoEngine: bozoengine_icon_*.png, bozoengine_banner.png
  python make_logo.py port <name> "<TITLE>" "<SUBTITLE>" [fire]
                                           any port: <name>_icon_1024/256.png and <name>_banner.png (the team clown,
                                           with fire hair when 'fire' is given)
Files go to BOZO_LOGO_OUT (default: docs/branding next to this repository's tools/); the font is BOZO_LOGO_FONT."""


def port_set(name, title, subtitle, fire=False, title_size=0.3):
    os.makedirs(OUT, exist_ok=True)
    make_icon = icon_tmb if fire else icon
    make_icon(1024).save(os.path.join(OUT, name + '_icon_1024.png'))
    make_icon(256).save(os.path.join(OUT, name + '_icon_256.png'))
    wordmark(2000, 600, False, title=title, subtitle=subtitle, fire=fire, title_size=title_size).save(
        os.path.join(OUT, name + '_banner.png'))
    print(name, 'done ->', os.path.abspath(OUT))


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] in ('-h', '--help'):
        print(USAGE)
    elif args and args[0] == 'tmb':
        port_set('tmb', 'TWISTED METAL: BLACK', 'NATIVE PC PORT  \u00b7  THE BOZOS', fire=True)
    elif args and args[0] == 'engine':
        port_set('bozoengine', 'BOZOENGINE', 'NATIVE PS2 PORT TOOLCHAIN  \u00b7  THE BOZOS', title_size=0.33)
    elif args and args[0] == 'port' and len(args) >= 4:
        port_set(args[1], args[2], args[3], fire=len(args) > 4 and args[4] == 'fire')
    elif not args:
        os.makedirs(OUT, exist_ok=True)
        icon(1024).save(os.path.join(OUT, 'bozos_icon_1024.png'))
        icon(256).save(os.path.join(OUT, 'bozos_icon_256.png'))
        wordmark(2000, 600, False).save(os.path.join(OUT, 'bozos_banner.png'))
        wordmark(2000, 600, True).save(os.path.join(OUT, 'bozos_wordmark_transparent.png'))
        print('team set done ->', os.path.abspath(OUT))
    else:
        print(USAGE)
