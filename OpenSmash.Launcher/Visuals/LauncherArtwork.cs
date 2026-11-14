// Original Windows drawing layouts, now maintained in the shared launcher.
using System.Drawing;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        void PaintHome(Graphics g, int W, int H)
        {
            float s = _s, t = (float)(Now - _heroAt), left = 104 * s;

            // .hero-vert
            {
                var st = PushFade(g, _shownAt < 0 ? 0 : Up((float)(Now - _shownAt), 0.2f), 0);
                var vs = g.Save();
                g.TranslateTransform(40 * s, H / 2f);
                g.RotateTransform(-90);
                ArtworkTheme.Label(g, "NATIVE / PC / OPEN SOURCE", FM(12 * s), A(ArtworkTheme.Dim), -300 * s, -8 * s, 16 * s, 6 * s, StringAlignment.Center, 600 * s);
                g.Restore(vs);
                PopFade(g, st);
            }

            // .kicker
            {
                var st = PushFade(g, Up(t, 0.2f), 24 * s);
                float ky = 128 * s, kh = 16 * s;
                bool lit = Now % 1.6 < 0.8;
                using (var b = new SolidBrush(A(Color.FromArgb(lit ? 255 : 51, ArtworkTheme.Accent))))
                    g.FillPolygon(b, new[] { new PointF(left + 3 * s, ky + 4 * s), new PointF(left + 11 * s, ky + 4 * s), new PointF(left + 8 * s, ky + 12 * s), new PointF(left, ky + 12 * s) });
                string kicker = $"NATIVE RECOMPILATION  \u00B7  GAME {(_version != "" ? _version : "?")}  \u00B7  {PlatformCaption}";
                ArtworkTheme.Label(g, kicker, FM(13 * s), A(ArtworkTheme.Muted), left + 21 * s, ky, kh, 13 * s * 0.14f);
                PopFade(g, st);
            }

            // .hero-title: each word rises out of its line with a skew
            float px = 150 * s, capTop = 160 * s, step = 0.86f * px;
            float openW = DrawHeroWord(g, "OPEN", left, capTop, px, t, 0.35f, ArtworkTheme.Text, false);
            DrawHeroWord(g, "SMASH", left + openW, capTop, px, t, 0.47f, ArtworkTheme.Accent, false);
            DrawHeroWord(g, "ULTIMATE", left + 110 * s, capTop + step, px, t, 0.60f, ArtworkTheme.Text, true);

            // .tagline
            {
                var st = PushFade(g, Up(t, 0.9f), 24 * s);
                DrawRich(g, new[]
                {
                    ("Recompiled, not emulated. ", FB(19 * s, true), ArtworkTheme.Text),
                    ($"Super Smash Bros. Ultimate as a real {PlatformName} program, built on this PC from your own copy.", FB(19 * s), ArtworkTheme.Muted),
                }, left, 420 * s, 600 * s, 30 * s);
                PopFade(g, st);
            }

            // .cta-row: < PLAY > (same mode as the header), MODS, SETTINGS, QUIT
            {
                var st = PushFade(g, Up(t, 1.05f), 24 * s);
                float bx = left, by = 494 * s, bh = 52 * s, aw = bh, ag = 8 * s;
                float[] widths = { 220, 150, 184, 128 };
                string playLabel = _launchMode == "vanilla" ? "VANILLA" : "PLAY";
                bool playEnabled = _built && !Busy;
                ArtworkTheme.ButtonAnimated(g, "<", bx, by, aw, bh, s, primary: false, Hov("nav:mode-prev", _hover == "nav:mode-prev"), _built, 18 * s, _alpha, press: Press("nav:mode-prev"));
                AddHit("nav:mode-prev", bx, by, aw, bh);
                bx += aw + ag;
                for (int i = 0; i < _menuItems.Length; i++)
                {
                    string id = $"menu:{i}";
                    float bw = widths[i] * s;
                    bool enabled = i != 0 || playEnabled;
                    float hv = Hov(id, enabled && (_hover == id || (_keys && _sel == i)));
                    ArtworkTheme.ButtonAnimated(g, i == 0 ? playLabel : _menuItems[i], bx, by, bw, bh, s, primary: i == 0, hv, enabled, 15 * s, _alpha,
                        i == 0 ? DrawArrow : null, press: Press(id));
                    if (enabled) AddHit(id, bx, by, bw, bh);
                    bx += bw;
                    if (i == 0)
                    {
                        bx += ag;
                        ArtworkTheme.ButtonAnimated(g, ">", bx, by, aw, bh, s, primary: false, Hov("nav:mode-next", _hover == "nav:mode-next"), _built, 18 * s, _alpha, press: Press("nav:mode-next"));
                        AddHit("nav:mode-next", bx, by, aw, bh);
                        bx += aw;
                    }
                    bx += 14 * s;
                }
                PopFade(g, st);
            }

            // .chips
            {
                var st = PushFade(g, Up(t, 1.2f), 24 * s);
                float cx = left, cy = 576 * s, chh = 28 * s;
                var f = FM(12 * s);
                float sp = 12 * s * 0.06f;
                foreach (string chip in new[] { RendererChip(), $"{_mods.Count} MOD{(_mods.Count == 1 ? "" : "S")}", $"{_plugins} PLUGIN{(_plugins == 1 ? "" : "S")}", "F1  IN-GAME MENU" })
                {
                    float cw = ArtworkTheme.TextWidth(g, chip, f, sp) + 26 * s;
                    using (var p = ArtworkTheme.Para(cx, cy, cw, chh, 6 * s))
                    {
                        using (var b = new SolidBrush(A(White(8)))) g.FillPath(b, p);
                        using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                    }
                    ArtworkTheme.Label(g, chip, f, A(ArtworkTheme.Muted), cx, cy, chh, sp, StringAlignment.Center, cw);
                    cx += cw + 8 * s;
                }
                PopFade(g, st);
            }
        }

        void DrawArrow(Graphics g, Color c, float x, float cy)
        {
            float u = 18 * _s / 24f;
            using var pen = new Pen(c, 2.5f * u) { LineJoin = LineJoin.Miter };
            g.DrawLine(pen, x + 0 * u, cy, x + 14 * u, cy);
            g.DrawLines(pen, new[] { new PointF(x + 7 * u, cy - 7 * u), new PointF(x + 14 * u, cy), new PointF(x + 7 * u, cy + 7 * u) });
        }

        float DrawHeroWord(Graphics g, string word, float x, float capTop, float px, float t, float delay, Color color, bool outline)
        {
            var path = TextPath(word, px);
            var b = path.GetBounds();
            float e = ArtworkTheme.Ease(Clamp01((t - delay) / 1f));
            if (e > 0)
            {
                float lineTop = capTop - 0.06f * px, lineH = b.Height + 0.12f * px;
                var st = g.Save();
                g.SetClip(new RectangleF(x - 10 * _s, lineTop, Advance(g, word, px) + 30 * _s, lineH), CombineMode.Intersect);
                g.TranslateTransform(x, capTop - b.Top + lineH * 1.05f * (1 - e));
                if (e < 1) g.MultiplyTransform(new Matrix(1, MathF.Tan(6 * MathF.PI / 180) * (1 - e), 0, 1, 0, 0));
                if (outline)
                {
                    using var pen = new Pen(color, Math.Max(1.5f, 2.6f * _s)) { LineJoin = LineJoin.Miter };
                    g.DrawPath(pen, path);
                }
                else
                {
                    using var br = new SolidBrush(color);
                    g.FillPath(br, path);
                }
                g.Restore(st);
            }
            return Advance(g, word, px);
        }

        void DrawRich(Graphics g, (string Text, Font Font, Color Color)[] runs, float x, float y, float maxW, float lineH)
        {
            float cx = x, cy = y;
            foreach (var run in runs)
            {
                using var b = new SolidBrush(A(run.Color));
                var (cap, top) = ArtworkTheme.Caps(run.Font);
                foreach (string word in run.Text.Split(' '))
                {
                    if (word.Length == 0) continue;
                    string piece = word + " ";
                    float w = g.MeasureString(piece, run.Font, 100000, ArtworkTheme.Typo).Width;
                    if (cx + w - g.MeasureString(" ", run.Font, 100000, ArtworkTheme.Typo).Width > x + maxW && cx > x) { cx = x; cy += lineH; }
                    g.DrawString(piece, run.Font, b, cx, cy + (lineH - cap) / 2 - top, ArtworkTheme.Typo);
                    cx += w;
                }
            }
        }

        void PaintHeader(Graphics g, int W, int H)
        {
            float s = _s;
            float e = _shownAt < 0 ? 0 : Up((float)(Now - _shownAt), 0, 0.6f);
            var st = PushFade(g, e, -10 * s);

            if (_view != "home")
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s)))
                    g.DrawLine(pen, 0, HeaderH * s, W, HeaderH * s);

            // Brand: the emblem tilts on hover (.brand:hover .brand-mark)
            float bh = ArtworkTheme.Ease(Hov("brand", _hover == "brand"));
            float mark = 34 * s, mx = 40 * s, my = (HeaderH * s - mark) / 2;
            if (_logo != null)
            {
                var ms = g.Save();
                g.TranslateTransform(mx + mark / 2, my + mark / 2);
                g.RotateTransform(-8 * bh);
                g.ScaleTransform(1 + 0.08f * bh, 1 + 0.08f * bh);
                g.DrawImage(_logo, new RectangleF((int)(-mark / 2), (int)(-mark / 2), (int)mark, (int)mark), _alpha);
                g.Restore(ms);
            }
            float wx = mx + mark + 10 * s;
            float ww = ArtworkTheme.Label(g, "OPENSMASH", FD(21 * s), A(ArtworkTheme.Text), wx, 0, HeaderH * s, 0.84f * s);
            float ww2 = ArtworkTheme.Label(g, "-ULTIMATE", FD(21 * s), A(ArtworkTheme.Dim), wx + ww + 0.84f * s, 0, HeaderH * s, 0.84f * s);
            AddHit("brand", mx, my, mark + 10 * s + ww + ww2, mark);

            // Window buttons
            float cx = W - 40 * s, cy = HeaderH * s / 2, a = 7 * s;
            float ch = Hov("close", _hover == "close");
            using (var pen = new Pen(A(ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Accent, ch)), Math.Max(1.4f, 1.6f * s)))
            {
                g.DrawLine(pen, cx - a, cy - a, cx + a, cy + a);
                g.DrawLine(pen, cx + a, cy - a, cx - a, cy + a);
            }
            AddHit("close", cx - 17 * s, cy - 17 * s, 34 * s, 34 * s);
            float nx = cx - 40 * s, mh = Hov("min", _hover == "min");
            using (var pen = new Pen(A(ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Text, mh)), Math.Max(1.4f, 1.6f * s)))
                g.DrawLine(pen, nx - a, cy, nx + a, cy);
            AddHit("min", nx - 17 * s, cy - 17 * s, 34 * s, 34 * s);

            // Call to action, with arrows for Play (mods) and Vanilla
            var cf = FB(13 * s, true);
            float csp = 13 * s * 0.12f;
            float labelW = Math.Max(ArtworkTheme.TextWidth(g, "PLAY", cf, csp), ArtworkTheme.TextWidth(g, "VANILLA", cf, csp));
            float cw = labelW + 44 * s, chh = 38 * s, gap = 6 * s, aw = chh;
            float ctaX = nx - 30 * s - aw - gap - cw;
            float prevX = ctaX - gap - aw, nextX = ctaX + cw + gap;
            string playLabel = _launchMode == "vanilla" ? "VANILLA" : "PLAY";
            bool playEnabled = _built && !Busy;
            ArtworkTheme.ButtonAnimated(g, "<", prevX, cy - chh / 2, aw, chh, s, primary: false, Hov("nav:mode-prev", _hover == "nav:mode-prev"), _built, 16 * s, _alpha, press: Press("nav:mode-prev"));
            ArtworkTheme.ButtonAnimated(g, playLabel, ctaX, cy - chh / 2, cw, chh, s, primary: true, Hov("nav:play", playEnabled && _hover == "nav:play"), playEnabled, 13 * s, _alpha, press: Press("nav:play"));
            ArtworkTheme.ButtonAnimated(g, ">", nextX, cy - chh / 2, aw, chh, s, primary: false, Hov("nav:mode-next", _hover == "nav:mode-next"), _built, 16 * s, _alpha, press: Press("nav:mode-next"));
            AddHit("nav:mode-prev", prevX, cy - chh / 2, aw, chh);
            if (playEnabled) AddHit("nav:play", ctaX, cy - chh / 2, cw, chh);
            AddHit("nav:mode-next", nextX, cy - chh / 2, aw, chh);

            // Section links with the red skewed underline
            string active = _view == "home" ? "nav:home" : _view == "settings" ? "nav:settings" : "nav:mods";
            float lx = prevX - 8 * s;
            foreach (var (id, label) in new[] { ("nav:settings", "SETTINGS"), ("nav:mods", "MODS"), ("nav:home", "HOME") })
            {
                float tw = ArtworkTheme.TextWidth(g, label, cf, csp), bw = tw + 24 * s;
                lx -= bw;
                bool on = id == active;
                float hv = Hov(id, _hover == id), u = on ? 1 : ArtworkTheme.Ease(hv);
                ArtworkTheme.Label(g, label, cf, A(on ? ArtworkTheme.Text : ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Text, hv)), lx + 12 * s, 0, HeaderH * s, csp);
                if (u > 0)
                {
                    float uy = cy + 13 * s, uw = tw * u, k = 2 * s * 0.577f;
                    using var b = new SolidBrush(A(ArtworkTheme.Accent));
                    g.FillPolygon(b, new[] { new PointF(lx + 12 * s + k, uy), new PointF(lx + 12 * s + uw + k, uy), new PointF(lx + 12 * s + uw, uy + 2 * s), new PointF(lx + 12 * s, uy + 2 * s) });
                }
                AddHit(id, lx, cy - 18 * s, bw, 36 * s);
            }

            PopFade(g, st);
        }

        void PaintStatus(Graphics g, int W, int H)
        {
            float s = _s, y = H - StatusH * s, h = StatusH * s;
            float e = _shownAt < 0 ? 0 : Up((float)(Now - _shownAt), 0.3f, 0.6f);
            var st = PushFade(g, e, 10 * s);

            using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s)))
                g.DrawLine(pen, 0, y, W, y);

            if (_shaderUi || _pluginBuildUi)
            {
                PaintJobStrip(g,W,y,h,s,_shaderUi?_shaderDetail:_pluginDetail,_shaderUi?_shaderFrac:_pluginFrac,_shaderUi?_shaderEta:_pluginEta);
                PopFade(g,st);return;
            }
            if (_loadOn && _view == "mods")
            {
                PaintJobStrip(g, W, y, h, s,
                    string.IsNullOrEmpty(_loadLabel) ? "WORKING" : _loadLabel,
                    _loadProgress, "");
                PopFade(g, st);
                return;
            }

            float x = 40 * s, dot = 8 * s, dy = y + h / 2;
            Color dc = _built ? ArtworkTheme.Ready : ArtworkTheme.Bad;
            if (_built)
            {
                float ph = (float)(Now % 2.0 / 2.0);
                if (ph < 0.7f)
                {
                    float r = dot / 2 + 10 * s * (ph / 0.7f);
                    using var rb = new SolidBrush(A(Color.FromArgb((int)(150 * (1 - ph / 0.7f)), dc)));
                    g.FillEllipse(rb, x + dot / 2 - r, dy - r, r * 2, r * 2);
                }
            }
            using (var b = new SolidBrush(A(dc))) g.FillEllipse(b, x, dy - dot / 2, dot, dot);

            var fb = FM(11 * s, true);
            float sp = 11 * s * 0.12f;
            float lw = ArtworkTheme.Label(g, _built ? "READY" : "NOT BUILT", fb, A(ArtworkTheme.Text), x + dot + 10 * s, y, h, sp);
            string info = _built
                ? $"GAME {(_version != "" ? _version : "?")}  /  {_mods.Count} MOD{(_mods.Count == 1 ? "" : "S")}  /  {_plugins} PLUGIN{(_plugins == 1 ? "" : "S")}"
                : "RUN THE INSTALLER TO BUILD THE GAME";
            ArtworkTheme.Label(g, info, FM(11 * s), A(ArtworkTheme.Dim), x + dot + 10 * s + lw + 18 * s, y, h, sp);

            string hint = _view switch
            {
                "home" => "\u2190 \u2192  SELECT     ENTER  CONFIRM",
                "mods" => "\u2191 \u2193  SELECT     DEL  REMOVE     ESC  BACK",
                "settings" => "\u2190 \u2192  TABS     ENTER  CHANGE     ESC  BACK",
                _ => "ESC  BACK",
            };
            ArtworkTheme.Label(g, hint, FM(11 * s), A(ArtworkTheme.Dim), 0, y, h, sp, StringAlignment.Far, W - 40 * s);

            PopFade(g, st);
        }

        void PaintJobStrip(Graphics g, int W, float y, float h, float s, string detail, float frac, string eta)
        {
            float barH = Math.Max(6, 8 * s);
            using (var track = new SolidBrush(A(White(36))))
                g.FillRectangle(track, 0, y, W, barH);
            if (frac < 0)
            {
                float span = 0.28f * W;
                float t = (float)(Now % 1.3 / 1.3);
                float x0 = t * (W + span) - span;
                float c0 = Math.Max(0, x0), c1 = Math.Min(W, x0 + span);
                if (c1 > c0)
                {
                    using var fill = new SolidBrush(A(ArtworkTheme.Accent));
                    g.FillRectangle(fill, c0, y, c1 - c0, barH);
                }
            }
            else
            {
                using var fill = new SolidBrush(A(ArtworkTheme.Accent));
                g.FillRectangle(fill, 0, y, Math.Max(barH, W * Math.Clamp(frac, 0f, 1f)), barH);
            }

            string step = string.IsNullOrEmpty(detail) ? "Working" : detail;
            if (step.Length > 0) step = char.ToUpper(step[0]) + step.Substring(1);
            string right = frac < 0 ? "" : $"{(int)Math.Round(frac * 100)}%   {eta}".Trim();
            var font = FM(12 * s);
            float pad = 28 * s;
            float rw = string.IsNullOrEmpty(right) ? 0 : g.MeasureString(right, font).Width;
            float maxW = Math.Max(40 * s, W - pad * 2 - rw - 18 * s);
            string full = step;
            while (step.Length > 12 && g.MeasureString(step, font).Width > maxW)
                step = step.Substring(0, step.Length - 2);
            if (step.Length < full.Length)
                step = step.TrimEnd() + "\u2026";

            float textTop = y + barH;
            float textH = Math.Max(1, h - barH);
            float sp = 12 * s * 0.04f;
            ArtworkTheme.Label(g, step, font, A(ArtworkTheme.Text), pad, textTop, textH, sp);
            if (right.Length > 0)
                ArtworkTheme.Label(g, right, font, A(ArtworkTheme.Muted), 0, textTop, textH, sp, StringAlignment.Far, W - pad);
        }

        void EnsureField(int W, int H)
        {
            if (_fieldSize.Width == W && _fieldSize.Height == H) return;
            _fieldSize = new Size(W, H);
            int n = (int)Math.Round(Math.Min(70, W / _s / 18));
            _lines = new SpeedLine[n];
            for (int i = 0; i < n; i++) _lines[i] = NewLine(true);
            _sparks = new Spark[(int)Math.Round(n * 0.9)];
            for (int i = 0; i < _sparks.Length; i++) _sparks[i] = NewSpark(true);
        }

        SpeedLine NewLine(bool anywhere)
        {
            float W = _fieldSize.Width, H = _fieldSize.Height, len = (60 + Rnd() * 260) * _s;
            return new SpeedLine
            {
                X = anywhere ? Rnd() * W * 1.4f - W * 0.2f : W + len + Rnd() * W * 0.3f,
                Y = Rnd() * H * 1.4f - H * 0.2f,
                Len = len,
                V = (6 + Rnd() * 16) * _s,
                W = Math.Max(1, (Rnd() < 0.12f ? 2 : 1) * _s),
                A = 0.08f + Rnd() * 0.32f,
                Red = Rnd() < 0.08f,
                Z = 0.4f + Rnd(),
            };
        }

        Spark NewSpark(bool anywhere)
        {
            float W = _fieldSize.Width, H = _fieldSize.Height;
            return new Spark
            {
                X = Rnd() * W,
                Y = anywhere ? Rnd() * H : H + 10 * _s,
                Sz = Math.Max(1, (0.6f + Rnd() * 1.8f) * _s),
                Vy = (0.2f + Rnd() * 0.9f) * _s,
                Vx = (Rnd() - 0.5f) * 0.4f * _s,
                A = 0.2f + Rnd() * 0.6f,
                Z = 0.3f + Rnd(),
                Red = Rnd() < 0.25f,
            };
        }

        void StepField(float frames)
        {
            float W = _fieldSize.Width;
            for (int i = 0; i < _lines.Length; i++)
            {
                _lines[i].X -= _lines[i].V * frames;
                if (_lines[i].X + _lines[i].Len < -W * 0.3f) _lines[i] = NewLine(false);
            }
            for (int i = 0; i < _sparks.Length; i++)
            {
                _sparks[i].Y -= _sparks[i].Vy * frames;
                _sparks[i].X += _sparks[i].Vx * frames;
                if (_sparks[i].Y < -10 * _s) _sparks[i] = NewSpark(false);
            }
        }

        void DrawField(Graphics g, int W, int H, float intensity)
        {
            foreach (var l in _lines)
            {
                float px = l.X + _tx * 30 * l.Z * _s, py = l.Y + _ty * 20 * l.Z * _s;
                float x2 = px + Cos14 * l.Len, y2 = py + Sin14 * l.Len;
                if (px > W + 4 || x2 < -4 || Math.Max(py, y2) < -4 || Math.Min(py, y2) > H + 4) continue;
                Color c = l.Red ? ArtworkTheme.Accent : Color.White;
                using var pen = new Pen(Color.FromArgb((int)(l.A * 200 * intensity), c), l.W);
                g.DrawLine(pen, px, py, x2, y2);
            }
            foreach (var sp in _sparks)
            {
                Color c = sp.Red ? Color.FromArgb((int)(sp.A * 255 * intensity), 255, 90, 70) : White((int)(sp.A * 0.8f * 255 * intensity));
                using var b = new SolidBrush(c);
                g.FillRectangle(b, sp.X + _tx * 50 * sp.Z * _s, sp.Y + _ty * 30 * sp.Z * _s, sp.Sz, sp.Sz);
            }
        }

        void DrawSlashes(Graphics g, int W, int H, float t)
        {
            bool settled = t > Slashes.Max(sl => sl.Delay) + 0.95f;
            int m = (int)(SlashMargin * _s);
            if (settled && (_slashCache == null || _slashCache.Width != W + 2 * m || _slashCache.Height != H + 2 * m))
            {
                _slashCache?.Dispose();
                _slashCache = new Bitmap(W + 2 * m, H + 2 * m);
                using var cg = Graphics.FromImage(_slashCache);
                cg.TranslateTransform(m, m);
                DrawSlashBars(cg, W, H, 0, 0, t, glint: false);
            }
            if (settled)
            {
                g.DrawImageUnscaled(_slashCache!, (int)Math.Round(_tx * 24 * _s) - m, (int)Math.Round(_ty * 14 * _s) - m);
                DrawSlashBars(g, W, H, _tx * 24 * _s, _ty * 14 * _s, t, glint: true, bars: false);
            }
            else
            {
                _slashCache?.Dispose();
                _slashCache = null;
                DrawSlashBars(g, W, H, _tx * 24 * _s, _ty * 14 * _s, t, glint: true);
            }
        }

        void DrawSlashBars(Graphics g, int W, int H, float ox, float oy, float t, bool glint, bool bars = true)
        {
            var st = g.Save();
            g.TranslateTransform(W / 2f + ox, H / 2f + oy);
            g.RotateTransform(-14);
            g.TranslateTransform(-W / 2f, -H / 2f);
            float cw = W * 1.2f, ch = H * 1.4f, x0 = -W * 0.1f, y0 = -H * 0.2f;
            float left = x0 - 0.1f * cw, full = 1.3f * cw;
            foreach (var sl in Slashes)
            {
                float e = ArtworkTheme.Ease(Clamp01((t - sl.Delay) / 0.9f));
                if (e <= 0) continue;
                float y = y0 + sl.Top * ch, h = Math.Max(1, sl.H * _s), w = full * e;
                Color c = sl.Red ? ArtworkTheme.Accent : ArtworkTheme.Text;

                float glow = 40 * _s, ga = (sl.Red ? 0.6f : 0.35f) * sl.Op;
                var gr = new RectangleF(left, y - glow, w, h + glow * 2);
                if (bars)
                using (var gb = new LinearGradientBrush(gr, Color.Transparent, Color.Transparent, LinearGradientMode.Vertical))
                {
                    float mid = 0.5f, half = h / 2 / gr.Height;
                    gb.InterpolationColors = new ColorBlend(6)
                    {
                        Colors = new[] { Color.FromArgb(0, c), Color.FromArgb((int)(40 * ga), c), Color.FromArgb((int)(150 * ga), c),
                                         Color.FromArgb((int)(150 * ga), c), Color.FromArgb((int)(40 * ga), c), Color.FromArgb(0, c) },
                        Positions = new[] { 0f, mid - half - 0.22f, mid - half, mid + half, mid + half + 0.22f, 1f }
                    };
                    g.FillRectangle(gb, gr);
                }
                if (bars)
                    using (var b = new SolidBrush(Color.FromArgb((int)(255 * sl.Op), c)))
                        g.FillRectangle(b, left, y, w, h);

                if (glint && t > 1.6f)
                {
                    float ph = (float)(((t - 1.6f) % 4.5f) / 4.5f);
                    if (ph < 0.7f)
                    {
                        float op = ph < 0.1f ? ph / 0.1f : ph > 0.6f ? (0.7f - ph) / 0.1f : 1f;
                        float gw = full * 0.25f, gx = left + gw * (-0.3f + 4.5f * (ph / 0.7f));
                        using var path = new GraphicsPath();
                        var r = new RectangleF(gx, y - 10 * _s, gw, h + 20 * _s);
                        path.AddEllipse(r);
                        using var pb = new PathGradientBrush(path)
                        {
                            CenterColor = White((int)(230 * op * sl.Op)),
                            SurroundColors = new[] { Color.FromArgb(0, 255, 255, 255) },
                        };
                        g.FillEllipse(pb, r);
                    }
                }
            }
            g.Restore(st);
        }

        Font Fnt(int kind, float px)
        {
            var key = (kind, (int)MathF.Round(px * 4));
            if (!_fontCache.TryGetValue(key, out var f))
            {
                f = kind switch
                {
                    0 => ArtworkTheme.Display(px),
                    1 => ArtworkTheme.Body(px),
                    2 => ArtworkTheme.Body(px, true),
                    3 => ArtworkTheme.Mono(px),
                    _ => ArtworkTheme.Mono(px, true),
                };
                _fontCache[key] = f;
            }
            return f;
        }

        GraphicsPath TextPath(string text, float px)
        {
            var key = (text, (int)MathF.Round(px * 4));
            if (!_pathCache.TryGetValue(key, out var p))
            {
                p = new GraphicsPath();
                p.AddString(text, FD(px).FontFamily, (int)FontStyle.Regular, px, PointF.Empty, ArtworkTheme.Typo);
                _pathCache[key] = p;
            }
            return p;
        }

        void PaintSectionHead(Graphics g, string eyebrow, string title)
        {
            float s = _s, t = (float)(Now - _viewAt), x = Gutter * s;

            {
                var st = PushFade(g, Up(t, 0), 24 * s);
                float ey = 86 * s, eh = 16 * s, k = 2 * s * 0.577f;
                float bw = 96 * s, bh = 32 * s, by = (HeaderH + 6) * s;
                ArtworkTheme.ButtonAnimated(g, "BACK", x, by, bw, bh, s, primary: false, Hov("nav:back", _hover == "nav:back"), true, 12 * s, _alpha, press: Press("nav:back"));
                AddHit("nav:back", x, by, bw, bh);
                float ex = x + bw + 18 * s;
                using (var b = new SolidBrush(A(ArtworkTheme.Accent)))
                    g.FillPolygon(b, new[] { new PointF(ex + k, ey + eh / 2 - s), new PointF(ex + 34 * s + k, ey + eh / 2 - s), new PointF(ex + 34 * s, ey + eh / 2 + s), new PointF(ex, ey + eh / 2 + s) });
                ArtworkTheme.Label(g, eyebrow, FM(13 * s), A(ArtworkTheme.Accent), ex + 46 * s, ey, eh, 13 * s * 0.2f);
                PopFade(g, st);
            }

            float px = 66 * s, capTop = SectionTitleTop * s;
            var path = TextPath(title, px);
            var dot = TextPath(".", px);
            var b0 = path.GetBounds();
            float adv = Advance(g, title, px), full = adv + Advance(g, ".", px);

            float tr = ArtworkTheme.Ease(Clamp01((t - 0.2f) / 0.9f));
            float op = Clamp01((t - 0.35f) / 0.4f);
            float boxTop = capTop - 0.08f * px, boxH = b0.Height + 0.16f * px;

            var st2 = g.Save();
            g.TranslateTransform(x - 40 * s * (1 - tr), capTop - b0.Top);
            if (tr < 1)
            {
                float cy = b0.Top + b0.Height / 2;
                float sk = MathF.Tan(-10 * MathF.PI / 180) * (1 - tr);
                g.MultiplyTransform(new Matrix(1, 0, sk, 1, -sk * cy, 0));
            }
            if (op > 0)
            {
                using (var b = new SolidBrush(ArtworkTheme.Fade(ArtworkTheme.Text, op))) g.FillPath(b, path);
                var ds = g.Save();
                g.TranslateTransform(adv, 0);
                using (var b = new SolidBrush(ArtworkTheme.Fade(ArtworkTheme.Accent, op))) g.FillPath(b, dot);
                g.Restore(ds);
            }

            float wt = t / 0.85f;
            if (wt < 1)
            {
                float bl = -0.02f * full, bw = 1.04f * full, wl, wr;
                if (wt <= 0.45f) { wl = bl; wr = bl + bw * ArtworkTheme.Ease(wt / 0.45f); }
                else { wl = bl + bw * ArtworkTheme.Ease(Clamp01((wt - 0.46f) / 0.54f)); wr = bl + bw; }
                float top = boxTop - capTop + b0.Top, hgt = boxH;
                using var b = new SolidBrush(ArtworkTheme.Text);
                g.FillRectangle(b, wl, top + 0.04f * hgt, Math.Max(0, wr - wl), hgt * 0.92f);
            }
            g.Restore(st2);
        }

        float Badge(Graphics g, string text, float x, float y, Color bg, Color fg, bool outline = false)
        {
            var f = FM(11 * _s, true);
            float sp = 11 * _s * 0.12f, h = 20 * _s;
            float w = ArtworkTheme.TextWidth(g, text, f, sp) + 20 * _s;
            using (var p = ArtworkTheme.Para(x, y, w, h, 5 * _s))
            {
                if (outline) using (var pen = new Pen(A(ArtworkTheme.Dim), Math.Max(1, _s))) g.DrawPath(pen, p);
                else using (var b = new SolidBrush(A(bg))) g.FillPath(b, p);
            }
            ArtworkTheme.Label(g, text, f, A(fg), x, y, h, sp, StringAlignment.Center, w);
            return w;
        }

        void HLine(Graphics g, float x1, float x2, float y)
        {
            using var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, _s));
            g.DrawLine(pen, x1, y, x2, y);
        }

        GraphicsState PushFade(Graphics g, float e, float dy)
        {
            var st = g.Save();
            g.TranslateTransform(0, dy * (1 - e));
            _alpha = e;
            return st;
        }

        void PopFade(Graphics g, GraphicsState st)
        {
            g.Restore(st);
            _alpha = 1;
        }

        void PaintCard(Graphics g, string id, float x, float y, float w, float h, int reveal, bool keyHot,
            string badge, Color badgeBg, Color badgeFg, string title, string desc)
        {
            float s = _s;
            var st = PushReveal(g, reveal);
            bool live = id != "";
            float hv = live ? Hov(id, _hover == id || keyHot) : 0, e = ArtworkTheme.Ease(hv);
            y -= 6 * s * e;
            float cut = 22 * s;
            using var path = ArtworkTheme.CutCorner(x, y, w, h, cut);
            using (var b = new SolidBrush(A(ArtworkTheme.Mix(ArtworkTheme.Bg2, ArtworkTheme.Bg3, e)))) g.FillPath(b, path);

            if (e > 0 && e < 1)
            {
                // .card::before: a light band sweeps across on hover
                var cs = g.Save();
                g.SetClip(path, CombineMode.Intersect);
                float bw = w * 0.4f, bx = x - 0.6f * w + (-1 + 6.2f * e) * bw;
                var band = new RectangleF(bx, y - h, bw, h * 3);
                g.TranslateTransform(bx + bw / 2, y + h / 2);
                g.RotateTransform(20);
                g.TranslateTransform(-(bx + bw / 2), -(y + h / 2));
                using var lg = new LinearGradientBrush(band, Color.Transparent, Color.Transparent, LinearGradientMode.Horizontal)
                {
                    InterpolationColors = new ColorBlend(3) { Colors = new[] { White(0), White(23), White(0) }, Positions = new[] { 0f, 0.5f, 1f } }
                };
                g.FillRectangle(lg, band);
                g.Restore(cs);
            }

            using (var pen = new Pen(A(ArtworkTheme.Mix(ArtworkTheme.Line, White(102), e)), Math.Max(1, s))) g.DrawPath(pen, path);
            using (var pen = new Pen(A(ArtworkTheme.Dim), Math.Max(1, 1.5f * s))) g.DrawLine(pen, x + w - cut, y, x + w, y + cut);

            float pad = 24 * s;
            Badge(g, badge, x + pad, y + 20 * s, badgeBg, badgeFg, badgeBg == Color.Empty);
            float titlePx = 30 * s;
            float maxTitleW = w - pad - 44 * s;
            while (titlePx > 18 * s && ArtworkTheme.TextWidth(g, title, FD(titlePx), 0.3f * s) > maxTitleW) titlePx -= 1 * s;
            ArtworkTheme.Label(g, title, FD(titlePx), A(ArtworkTheme.Text), x + pad, y + 52 * s, 26 * s, 0.3f * s);
            ArtworkTheme.DrawText(g, desc, FB(14 * s), A(ArtworkTheme.Muted), x + pad, y + 88 * s, w - pad * 2 - 40 * s, 20 * s);
            if (live)
            {
                float ax = x + w - 44 * s + 6 * s * e;
                DrawArrow(g, A(ArtworkTheme.Mix(ArtworkTheme.Dim, ArtworkTheme.Accent, e)), ax, y + h / 2 + 8 * s);
                AddHit(id, x, y, w, h);
            }
            PopFade(g, st);
        }

        void RowWash(Graphics g, float x, float y, float w, float h, float e, float strength = 0.14f)
        {
            if (e <= 0) return;
            float ww = Math.Max(1, w * e);
            using var lg = new LinearGradientBrush(new RectangleF(x - 1, y, ww + 2, h), A(Color.FromArgb((int)(255 * strength), ArtworkTheme.Accent)), Color.FromArgb(0, ArtworkTheme.Accent), LinearGradientMode.Horizontal);
            g.FillRectangle(lg, x, y, ww, h);
        }
}
