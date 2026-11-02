using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    // ============================================================================
    // Theme: the palette, fonts, background and drawing primitives shared by the
    // launcher and the installer so both windows look like one app. The design is
    // the OpenSmash site's (denkishi.github.io/opensmash-site): black, white
    // diagonal cuts, one red accent, Anton for display type.
    // ============================================================================
    static class Theme
    {
        public static readonly Color Bg      = Color.FromArgb(255, 5, 5, 5);         // --bg
        public static readonly Color Bg2     = Color.FromArgb(255, 13, 13, 14);      // --bg-2
        public static readonly Color Bg3     = Color.FromArgb(255, 21, 21, 23);      // --bg-3
        public static readonly Color Text    = Color.FromArgb(255, 244, 244, 242);   // --fg
        public static readonly Color Muted   = Color.FromArgb(255, 180, 180, 176);   // --fg-dim
        public static readonly Color Dim     = Color.FromArgb(255, 138, 138, 134);   // --fg-mute
        public static readonly Color Accent  = Color.FromArgb(255, 255, 59, 48);     // --red
        public static readonly Color Accent2 = Color.FromArgb(255, 212, 42, 32);     // darker red
        public static readonly Color Ready   = Color.FromArgb(255, 61, 255, 138);    // status "up"
        public static readonly Color Bad     = Color.FromArgb(255, 255, 107, 97);    // red, readable as text
        public static readonly Color Warn    = Color.FromArgb(255, 255, 194, 26);    // --amber
        public static readonly Color Info    = Color.FromArgb(255, 59, 224, 255);    // --cyan
        public static readonly Color Line    = Color.FromArgb(36, 255, 255, 255);    // --line

        // ---------------------------------------------------------------------------
        // Fonts. Anton ships inside the exe (SIL OFL, Assets\Anton-OFL.txt); body and
        // mono fall back the way the site's CSS does.
        static readonly PrivateFontCollection Embedded = new();
        static readonly FontFamily? Anton = LoadEmbeddedFont("Anton-Regular.ttf");
        static readonly HashSet<string> Installed = new InstalledFontCollection().Families.Select(f => f.Name).ToHashSet();
        static readonly string BodyFamily = Installed.Contains("Inter") ? "Inter" : "Segoe UI";
        static readonly string MonoFamily = new[] { "JetBrains Mono", "Cascadia Mono", "Consolas" }.First(n => Installed.Contains(n) || n == "Consolas");

        static FontFamily? LoadEmbeddedFont(string resource)
        {
            try
            {
                using var s = typeof(Theme).Assembly.GetManifestResourceStream(resource);
                if (s == null) return null;
                var data = new byte[s.Length];
                s.ReadExactly(data);
                // GDI+ keeps reading the font from this memory, so it is never freed.
                IntPtr mem = Marshal.AllocCoTaskMem(data.Length);
                Marshal.Copy(data, 0, mem, data.Length);
                Embedded.AddMemoryFont(mem, data.Length);
                return Embedded.Families.LastOrDefault();
            }
            catch { return null; }
        }

        public static Font Display(float px) =>
            Anton != null
                ? new Font(Anton, px, FontStyle.Regular, GraphicsUnit.Pixel)
                : new Font(Installed.Contains("Bahnschrift SemiBold Condensed") ? "Bahnschrift SemiBold Condensed" : "Segoe UI", px, FontStyle.Regular, GraphicsUnit.Pixel);

        public static Font Body(float px, bool bold = false) =>
            new Font(BodyFamily, px, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);

        public static Font Mono(float px, bool bold = false) =>
            new Font(MonoFamily, px, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);

        // cubic-bezier(.2, .8, .2, 1): the site's --ease
        public static float Ease(float t)
        {
            if (t <= 0) return 0;
            if (t >= 1) return 1;
            float u = t;
            for (int i = 0; i < 6; i++)
            {
                float x = Bez(u, 0.2f, 0.2f) - t;
                float d = BezD(u, 0.2f, 0.2f);
                if (MathF.Abs(d) < 1e-5f) break;
                u = Math.Clamp(u - x / d, 0f, 1f);
            }
            return Bez(u, 0.8f, 1f);
        }
        static float Bez(float u, float p1, float p2) { float v = 1 - u; return 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u; }
        static float BezD(float u, float p1, float p2) { float v = 1 - u; return 3 * v * v * p1 + 6 * v * u * (p2 - p1) + 3 * u * u * (1 - p2); }

        public static Color Fade(Color c, float alpha) =>
            Color.FromArgb(Math.Clamp((int)(c.A * alpha), 0, 255), c.R, c.G, c.B);

        public static Color Mix(Color a, Color b, float t) => Color.FromArgb(
            (int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));

        // ---------------------------------------------------------------------------
        // Text
        public static void DrawText(Graphics g, string text, Font font, Color color, float x, float y, float w, float h,
            StringAlignment align = StringAlignment.Near, StringAlignment valign = StringAlignment.Center)
        {
            if (string.IsNullOrEmpty(text)) return;
            using var sf = new StringFormat { Alignment = align, LineAlignment = valign, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
            using var b = new SolidBrush(color);
            g.DrawString(text, font, b, new RectangleF(x, y, Math.Max(1, w), Math.Max(1, h)), sf);
        }

        // Wrapped paragraph, clipped to the box (last visible line gets an ellipsis).
        public static void DrawWrapped(Graphics g, string text, Font font, Color color, float x, float y, float w, float h)
        {
            if (string.IsNullOrEmpty(text)) return;
            using var sf = new StringFormat { Alignment = StringAlignment.Near, LineAlignment = StringAlignment.Near, Trimming = StringTrimming.EllipsisWord, FormatFlags = StringFormatFlags.LineLimit };
            using var b = new SolidBrush(color);
            g.DrawString(text, font, b, new RectangleF(x, y, Math.Max(1, w), Math.Max(1, h)), sf);
        }

        // Paths are long; trim the middle so the drive and the file name both stay visible.
        public static void DrawPath(Graphics g, string text, Font font, Color color, float x, float y, float w, float h)
        {
            if (string.IsNullOrEmpty(text)) return;
            using var sf = new StringFormat { Alignment = StringAlignment.Near, LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisPath, FormatFlags = StringFormatFlags.NoWrap };
            using var b = new SolidBrush(color);
            g.DrawString(text, font, b, new RectangleF(x, y, Math.Max(1, w), Math.Max(1, h)), sf);
        }

        public static void DrawTextSpaced(Graphics g, string text, Font font, Color color, float x, float y, float extra)
        {
            using var b = new SolidBrush(color);
            DrawSpaced(g, text, font, b, x, y, extra);
        }

        // Typographic layout (no GDI+ padding) that still measures spaces.
        public static readonly StringFormat Typo = MakeTypo();
        static StringFormat MakeTypo()
        {
            var f = (StringFormat)StringFormat.GenericTypographic.Clone();
            f.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces | StringFormatFlags.NoWrap;
            return f;
        }

        static readonly Dictionary<(string, float, FontStyle, char), float> CharWidths = new();
        static float CharWidth(Graphics g, Font f, char c)
        {
            var key = (f.Name, f.Size, f.Style, c);
            if (!CharWidths.TryGetValue(key, out float w))
            {
                w = g.MeasureString(c.ToString(), f, 10000, Typo).Width;
                CharWidths[key] = w;
            }
            return w;
        }

        public static float TextWidth(Graphics g, string text, Font f, float spacing = 0)
        {
            if (string.IsNullOrEmpty(text)) return 0;
            if (spacing == 0) return g.MeasureString(text, f, 100000, Typo).Width;
            float w = 0;
            foreach (char c in text) w += CharWidth(g, f, c);
            return w + spacing * (text.Length - 1);
        }

        public static void DrawSpaced(Graphics g, string text, Font f, Brush b, float x, float y, float spacing)
        {
            if (string.IsNullOrEmpty(text)) return;
            if (spacing == 0) { g.DrawString(text, f, b, x, y, Typo); return; }
            float cx = x;
            foreach (char c in text)
            {
                if (c != ' ') g.DrawString(c.ToString(), f, b, cx, y, Typo);
                cx += CharWidth(g, f, c) + spacing;
            }
        }

        // Cap height and the em-box top of a font family, measured once from an "H".
        static readonly Dictionary<(string, FontStyle), (float Cap, float Top)> CapMetrics = new();
        public static (float Cap, float Top) Caps(Font f)
        {
            var key = (f.Name, f.Style);
            if (!CapMetrics.TryGetValue(key, out var m))
            {
                using var p = new GraphicsPath();
                p.AddString("H", f.FontFamily, (int)f.Style, 100, PointF.Empty, Typo);
                var b = p.GetBounds();
                m = (b.Height / 100f, b.Top / 100f);
                CapMetrics[key] = m;
            }
            return (m.Cap * f.Size, m.Top * f.Size);
        }

        // One line of uppercase-style text whose capitals are centred in (y, h).
        public static float Label(Graphics g, string text, Font f, Color color, float x, float y, float h,
            float spacing = 0, StringAlignment align = StringAlignment.Near, float w = 0)
        {
            if (string.IsNullOrEmpty(text) || color.A == 0) return 0;
            var (cap, top) = Caps(f);
            float ty = y + (h - cap) / 2 - top;
            float tw = TextWidth(g, text, f, spacing);
            float tx = align == StringAlignment.Center ? x + (w - tw) / 2 : align == StringAlignment.Far ? x + w - tw : x;
            using var b = new SolidBrush(color);
            DrawSpaced(g, text, f, b, tx, ty, spacing);
            return tw;
        }

        // ---------------------------------------------------------------------------
        // Shapes
        public static GraphicsPath RoundRect(float x, float y, float w, float h, float r)
        {
            var p = new GraphicsPath();
            float d = r * 2;
            if (d > w) d = w;
            if (d > h) d = h;
            if (d <= 0) { p.AddRectangle(new RectangleF(x, y, Math.Max(0.1f, w), Math.Max(0.1f, h))); return p; }
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d, y, d, d, 270, 90);
            p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            p.AddArc(x, y + h - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // The site's slanted shape: clip-path polygon(cut 0, 100% 0, calc(100% - cut) 100%, 0 100%).
        public static GraphicsPath Para(float x, float y, float w, float h, float cut)
        {
            var p = new GraphicsPath();
            p.AddPolygon(new[] { new PointF(x + cut, y), new PointF(x + w, y), new PointF(x + w - cut, y + h), new PointF(x, y + h) });
            return p;
        }

        // A card with its top-right corner cut off (.card).
        public static GraphicsPath CutCorner(float x, float y, float w, float h, float cut)
        {
            var p = new GraphicsPath();
            p.AddPolygon(new[] { new PointF(x, y), new PointF(x + w - cut, y), new PointF(x + w, y + cut), new PointF(x + w, y + h), new PointF(x, y + h) });
            return p;
        }

        // Top-right and bottom-left corners cut (.status, dialogs).
        public static GraphicsPath CutCorners(float x, float y, float w, float h, float cut)
        {
            var p = new GraphicsPath();
            p.AddPolygon(new[] { new PointF(x, y), new PointF(x + w - cut, y), new PointF(x + w, y + cut), new PointF(x + w, y + h),
                                 new PointF(x + cut, y + h), new PointF(x, y + h - cut) });
            return p;
        }

        // The red bar that fades out to the right (the running install step).
        public static void FillAccentBar(Graphics g, float x, float y, float w, float h, float r, int alpha)
        {
            using (var lg = new LinearGradientBrush(new RectangleF(x - 1, y, w + 2, h), Color.FromArgb(alpha / 2, Accent), Color.FromArgb(0, Accent), LinearGradientMode.Horizontal))
                g.FillRectangle(lg, x, y, w, h);
            using var b = new SolidBrush(Color.FromArgb(alpha, Accent));
            g.FillRectangle(b, x, y, Math.Max(2, h / 12), h);
        }

        // A dark card with the standard hairline border and the cut corner.
        public static void Card(Graphics g, float x, float y, float w, float h, float r, float s, int fillAlpha = 242)
        {
            float cut = 16 * s;
            using var p = CutCorner(x, y, w, h, cut);
            using (var b = new SolidBrush(Color.FromArgb(fillAlpha, Bg2)))
                g.FillPath(b, p);
            using (var pen = new Pen(Line, Math.Max(1, s)))
            {
                g.DrawPath(pen, p);
            }
            using (var pen = new Pen(Fade(Dim, 0.8f), Math.Max(1, 1.5f * s)))
                g.DrawLine(pen, x + w - cut, y, x + w, y + cut);
        }

        // Primary (white, red sweeps in on hover) or ghost (outlined, white sweeps in) button.
        public static void Button(Graphics g, string label, float x, float y, float w, float h, float s, bool primary, bool hot, bool enabled = true, float textPx = 0)
            => ButtonAnimated(g, label, x, y, w, h, s, primary, hot ? 1 : 0, enabled, textPx);

        public static void ButtonAnimated(Graphics g, string label, float x, float y, float w, float h, float s, bool primary, float hover,
            bool enabled = true, float textPx = 0, float alpha = 1, Action<Graphics, Color, float, float>? icon = null)
        {
            if (!enabled) { hover = 0; alpha *= 0.45f; }
            float e = Ease(hover);
            y -= 2 * s * e;
            float cut = Math.Min(12 * s, h * 0.24f);
            using var path = Para(x, y, w, h, cut);
            var state = g.Save();
            g.SetClip(path, CombineMode.Intersect);

            Color baseFill = primary ? Text : Color.FromArgb(16, 255, 255, 255);
            using (var b = new SolidBrush(Fade(baseFill, alpha))) g.FillPath(b, path);
            if (!primary)
                using (var pen = new Pen(Fade(Color.FromArgb(140, 255, 255, 255), alpha), 4 * s)) g.DrawPath(pen, path);

            if (e > 0)
            {
                // ::before: translateX(-110%) skewX(-20deg) -> none
                float off = -1.1f * w * (1 - e);
                float skew = h * 0.364f * (1 - e);
                var sweep = new[] { new PointF(x + off + skew, y), new PointF(x + off + w + skew + cut, y), new PointF(x + off + w + cut, y + h), new PointF(x + off, y + h) };
                using var b = new SolidBrush(Fade(primary ? Accent : Text, alpha));
                g.FillPolygon(b, sweep);
            }
            g.Restore(state);

            Color fg = primary ? Mix(Bg, Color.White, e) : Mix(Text, Bg, e);
            using var f = Body(textPx > 0 ? textPx : 14 * s, true);
            float sp = f.Size * 0.1f;
            string text = label.ToUpperInvariant();
            if (icon == null)
            {
                Label(g, text, f, Fade(fg, alpha), x, y, h, sp, StringAlignment.Center, w);
            }
            else
            {
                float iconW = h * 0.36f, gap = 10 * s;
                float tw = TextWidth(g, text, f, sp);
                float tx = x + (w - tw - gap - iconW) / 2;
                Label(g, text, f, Fade(fg, alpha), tx, y, h, sp);
                icon(g, Fade(fg, alpha), tx + tw + gap, y + h / 2);
            }
        }

        // ---------------------------------------------------------------------------
        // Background: near-black, a red glow upper right, a faint white glow lower left,
        // scanlines; optionally the two static slashes of the site's install section.
        public static Bitmap MakeBackground(int W, int H, Image? logo, bool slashes = true)
        {
            var bmp = Glows(W, H);
            using var g = Graphics.FromImage(bmp);
            g.SmoothingMode = SmoothingMode.HighQuality;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;

            if (slashes)
            {
                var st = g.Save();
                g.TranslateTransform(W / 2f, H / 2f);
                g.RotateTransform(-14);
                g.TranslateTransform(-W / 2f, -H / 2f);
                using (var b = new SolidBrush(Color.FromArgb(64, 255, 255, 255)))
                    g.FillRectangle(b, -W * 0.2f, H * 0.18f, W * 1.4f, Math.Max(1, H / 360f));
                using (var b = new SolidBrush(Color.FromArgb(8, 255, 255, 255)))
                    g.FillRectangle(b, -W * 0.2f, H * 0.78f - H / 6f, W * 1.4f, H / 6f);
                g.Restore(st);
            }

            using (var b = new SolidBrush(Color.FromArgb(3, 255, 255, 255)))
                for (int y = 0; y < H; y += 3)
                    g.FillRectangle(b, 0, y, W, 1);

            return bmp;
        }

        // Bg under two radial-gradient(ellipse rx ry at cx cy, color, transparent 70%) glows,
        // computed per pixel and dithered: a gradient brush this faint bands into visible rings.
        static Bitmap Glows(int W, int H)
        {
            var glows = new (float Cx, float Cy, float Rx, float Ry, float R, float G, float B, float A)[]
            {
                (W * 0.70f, H * 0.40f, W * 0.60f * 0.7f, H * 0.50f * 0.7f, 255, 59, 48, 0.16f),
                (W * 0.10f, H * 1.00f, W * 0.80f * 0.7f, H * 0.60f * 0.7f, 255, 255, 255, 0.06f),
            };
            var bmp = new Bitmap(W, H, PixelFormat.Format32bppArgb);
            var data = bmp.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            var row = new byte[W * 4];
            uint seed = 2463534242;
            for (int y = 0; y < H; y++)
            {
                for (int x = 0; x < W; x++)
                {
                    float r = Bg.R, gr = Bg.G, b = Bg.B;
                    foreach (var gl in glows)
                    {
                        float dx = (x + 0.5f - gl.Cx) / gl.Rx, dy = (y + 0.5f - gl.Cy) / gl.Ry;
                        float d = MathF.Sqrt(dx * dx + dy * dy);
                        if (d >= 1) continue;
                        float a = gl.A * (1 - d);
                        r += (gl.R - r) * a; gr += (gl.G - gr) * a; b += (gl.B - b) * a;
                    }
                    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                    float n = (seed & 0xFFFF) / 65536f;
                    int i = x * 4;
                    row[i] = (byte)Math.Min(255, (int)(b + n));
                    row[i + 1] = (byte)Math.Min(255, (int)(gr + n));
                    row[i + 2] = (byte)Math.Min(255, (int)(r + n));
                    row[i + 3] = 255;
                }
                System.Runtime.InteropServices.Marshal.Copy(row, 0, data.Scan0 + y * data.Stride, row.Length);
            }
            bmp.UnlockBits(data);
            return bmp;
        }

        // ---------------------------------------------------------------------------
        // Window chrome shared by both forms
        public static void CloseButton(Graphics g, int W, float s, bool hot, out RectangleF hit)
        {
            float size = 34 * s;
            float x = W - 24 * s - size;
            float y = 22 * s;
            float c = size / 2f, a = size * 0.22f;
            using var pen = new Pen(hot ? Accent : Muted, Math.Max(1.4f, 1.6f * s));
            g.DrawLine(pen, x + c - a, y + c - a, x + c + a, y + c + a);
            g.DrawLine(pen, x + c + a, y + c - a, x + c - a, y + c + a);
            hit = new RectangleF(x - 4 * s, y - 4 * s, size + 8 * s, size + 8 * s);
        }

        // The window size both forms use: 16:9, up to 810 px tall.
        public static Size WindowSize()
        {
            var wa = Screen.PrimaryScreen!.WorkingArea;
            int H = Math.Min((int)(wa.Height * 0.88), 810);
            int W = (int)(H * 16.0 / 9.0);
            if (W > wa.Width * 0.94) { W = (int)(wa.Width * 0.94); H = (int)(W * 9.0 / 16.0); }
            return new Size(W, H);
        }
    }
}
