using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Linq;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    // ============================================================================
    // LauncherForm drawing and animation, in the style of the OpenSmash site: the
    // hero's slashes, speed lines and sparks, the staggered entrance, the section
    // headings' wipe, slanted buttons and cut-corner cards. Every view is drawn in
    // launcher units (the window is 1280 x 720 of them, _s pixels each).
    // ============================================================================
    public partial class LauncherForm
    {
        const float Gutter = 96, HeaderH = 64, StatusH = 40, SectionTitleTop = 112, ContentTop = 184;

        Bitmap _bg;
        float _s;
        readonly string[] _menuItems = { "PLAY", "MODS", "SETTINGS", "QUIT" };

        Setting[] _settings;
        SettingTab[] _settingsTabs;
        int _settingsTab = 0;
        int _ddFirst;
        float _settingsScroll;
        float _settingsMaxScroll;
        float _settingsViewTop;
        float _settingsViewH;
        float _settingsRowH = 74;
        float[] _sectionTop = Array.Empty<float>();
        float[] _rowTop = Array.Empty<float>();
        bool _settingsThumbDrag;
        float _settingsThumbGrab;
        float _settingsTrackTop;
        float _settingsTrackH;
        float _settingsThumbH;
        bool _modThumbDrag;
        float _modThumbGrab;
        float _modTrackTop;
        float _modTrackH;
        float _modThumbH;
        int _modMaxScroll;

        static readonly Dictionary<string, string> SettingInfo = new()
        {
            ["window"] = "A window you can resize, borderless over the monitor, or full screen. F11 switches while playing.",
            ["window_size"] = "The inside of the window. The picture stays 16:9.",
            ["pacing"] = "Steady 60 on any monitor. G-Sync / FreeSync shows each frame as soon as it is ready.",
            ["render_scale"] = "How big the game draws. 2 is four times the pixels of 1.",
            ["gpu"] = "Auto picks the fastest card.",
            ["hide_cursor"] = "Hides the pointer over the game after 2 seconds without moving it.",
            ["overlay"] = "The frame rate in the corner. F1 shows or hides it while playing.",
            ["volume"] = "How loud the game is. 100 is full. It applies the next time you press Play.",
            ["name"] = "What other players see. Leave it empty and the game picks one.",
            ["lobby"] = "Public: everyone's rooms. Private: only players with the same code. Offline: no network.",
            ["code"] = "A word your friends type too. Capital letters count.",
            ["server"] = "host:port of a lobby you run, for example 192.168.1.20:39420.",
            ["direct"] = "On is faster. Off sends everything through the lobby, so your address stays private.",
            ["keyboard_player"] = "The keyboard adds to that player's controller.",
            ["background"] = "Off: controllers work only while the game's window is in front.",
            ["pads"] = "Pins and button layouts. Each controller is also set up in the game, under Controllers.",
        };

        // ---------------------------------------------------------------------------
        // Animation clock, hover progress and the pointer for parallax
        readonly Stopwatch _clock = Stopwatch.StartNew();
        System.Windows.Forms.Timer _animTimer;
        double _lastTick, _pendingDt;
        double _shownAt = -1, _heroAt, _viewAt, _promptAt, _ddAt;
        string _paintedView = "", _paintedDd;
        bool _paintedPrompt, _paintedPluginPrompt;
        double _pluginPromptAt;
        float _alpha = 1;                                        // multiplies every colour drawn through A()
        readonly Dictionary<string, float> _hov = new();         // hit id -> hover progress 0..1
        readonly HashSet<string> _hovOn = new();                 // hit ids hovered or selected this frame
        float _mx, _my, _tx, _ty;
        List<EmulatorInfo> _emus = new();

        double Now => _clock.Elapsed.TotalSeconds;
        Color A(Color c) => _alpha >= 1 ? c : Theme.Fade(c, _alpha);
        static Color White(int a) => Color.FromArgb(Math.Clamp(a, 0, 255), 255, 255, 255);

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            if (_animTimer != null) return;
            _animTimer = new System.Windows.Forms.Timer { Interval = 15 };
            _animTimer.Tick += OnAnimTick;
            _animTimer.Start();
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            _animTimer?.Stop();
            _animTimer?.Dispose();
            base.OnFormClosed(e);
        }

        protected override void OnShown(EventArgs e)
        {
            _shownAt = _heroAt = _viewAt = Now;
            base.OnShown(e);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (ClientSize.Width > 0 && ClientSize.Height > 0)
            {
                _mx = (e.X / (float)ClientSize.Width - 0.5f) * 2;
                _my = (e.Y / (float)ClientSize.Height - 0.5f) * 2;
            }
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            _mx = _my = 0;
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            if (_view == "settings" && _openDropdownKey != null)
            {
                _ddFirst -= Math.Sign(e.Delta);
                Invalidate();
            }
            else if (_view == "settings")
                ScrollSettings(_settingsScroll - Math.Sign(e.Delta) * _settingsRowH);
        }

        // The home buttons sit in a row, so Left / Right move through them like Up / Down.
        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (_view == "home" && (e.KeyCode == Keys.Left || e.KeyCode == Keys.Right))
            {
                base.OnKeyDown(new KeyEventArgs(e.KeyCode == Keys.Left ? Keys.Up : Keys.Down));
                return;
            }
            base.OnKeyDown(e);
        }

        bool ActivateNavHit(string id)
        {
            switch (id)
            {
                case "brand":
                case "nav:home": GoView("home"); return true;
                case "nav:mods": GoView("mods"); return true;
                case "nav:settings": GoView("settings"); return true;
                case "nav:back": GoBack(); return true;
                case "nav:play": StartPlay(); return true;
                case "nav:mode-prev": CycleLaunchMode(-1); return true;
                case "nav:mode-next": CycleLaunchMode(1); return true;
                case "min": WindowState = FormWindowState.Minimized; return true;
            }
            return false;
        }

        void OnAnimTick(object sender, EventArgs e)
        {
            double now = Now;
            _pendingDt += now - _lastTick;
            _lastTick = now;
            if (WindowState == FormWindowState.Minimized || !Visible) { _pendingDt = 0; return; }
            // In the background (the game in front, another app focused) it redraws at about 20 fps.
            if (ActiveForm != this && _pendingDt < 0.05) return;
            float dt = (float)Math.Min(0.1, _pendingDt);
            _pendingDt = 0;

            foreach (var key in _hov.Keys.ToList())
            {
                bool on = _hovOn.Contains(key);
                float v = _hov[key];
                v = on ? Math.Min(1, v + dt / 0.3f) : Math.Max(0, v - dt / 0.3f);
                if (v == 0 && !on) _hov.Remove(key); else _hov[key] = v;
            }
            float k = 1 - MathF.Pow(0.94f, dt * 60);
            _tx += (_mx - _tx) * k;
            _ty += (_my - _ty) * k;
            StepField(dt * 60);

            // The marketplace page covers everything below the heading; only that strip moves.
            if (_view == "mods_browser" && _marketplaceWebView != null && _marketplaceWebView.Visible)
                Invalidate(new Rectangle(0, 0, ClientSize.Width, (int)(ContentTop * _s)));
            else
                Invalidate();
        }

        float Hov(string id, bool on)
        {
            if (on) _hovOn.Add(id);
            return _hov.TryGetValue(id, out float v) ? v : 0;
        }

        static float Clamp01(float v) => Math.Clamp(v, 0f, 1f);
        static float Up(float t, float delay, float dur = 0.8f) => Theme.Ease(Clamp01((t - delay) / dur));

        // .reveal: opacity 0 -> 1, translateY(40px) -> none, 70 ms apart
        float Reveal(int i) => Theme.Ease(Clamp01(((float)(Now - _viewAt) - 0.2f - (i % 6) * 0.07f) / 0.9f));

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

        GraphicsState PushReveal(Graphics g, int i) => PushFade(g, Reveal(i), 40 * _s);

        // ---------------------------------------------------------------------------
        // Fonts and display-type outlines, cached per size
        readonly Dictionary<(int, int), Font> _fontCache = new();
        readonly Dictionary<(string, int), GraphicsPath> _pathCache = new();

        Font Fnt(int kind, float px)
        {
            var key = (kind, (int)MathF.Round(px * 4));
            if (!_fontCache.TryGetValue(key, out var f))
            {
                f = kind switch
                {
                    0 => Theme.Display(px),
                    1 => Theme.Body(px),
                    2 => Theme.Body(px, true),
                    3 => Theme.Mono(px),
                    _ => Theme.Mono(px, true),
                };
                _fontCache[key] = f;
            }
            return f;
        }
        Font FD(float px) => Fnt(0, px);
        Font FB(float px, bool bold = false) => Fnt(bold ? 2 : 1, px);
        Font FM(float px, bool bold = false) => Fnt(bold ? 4 : 3, px);

        GraphicsPath TextPath(string text, float px)
        {
            var key = (text, (int)MathF.Round(px * 4));
            if (!_pathCache.TryGetValue(key, out var p))
            {
                p = new GraphicsPath();
                p.AddString(text, FD(px).FontFamily, (int)FontStyle.Regular, px, PointF.Empty, Theme.Typo);
                _pathCache[key] = p;
            }
            return p;
        }

        void ClearScaleCaches()
        {
            foreach (var f in _fontCache.Values) f.Dispose();
            _fontCache.Clear();
            foreach (var p in _pathCache.Values) p.Dispose();
            _pathCache.Clear();
            _bg?.Dispose();
            _bg = null;
            _fieldSize = Size.Empty;
        }

        float Advance(Graphics g, string text, float px) => g.MeasureString(text, FD(px), 100000, Theme.Typo).Width;

        // ---------------------------------------------------------------------------
        // Speed lines and sparks (the site's hero canvas), drifting along -14 degrees
        struct SpeedLine { public float X, Y, Len, V, W, A, Z; public bool Red; }
        struct Spark { public float X, Y, Sz, Vy, Vx, A, Z; public bool Red; }

        SpeedLine[] _lines = Array.Empty<SpeedLine>();
        Spark[] _sparks = Array.Empty<Spark>();
        readonly Random _rng = new();
        Size _fieldSize;
        float Rnd() => (float)_rng.NextDouble();

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

        static readonly float Cos14 = MathF.Cos(-14 * MathF.PI / 180), Sin14 = MathF.Sin(-14 * MathF.PI / 180);

        void DrawField(Graphics g, int W, int H, float intensity)
        {
            foreach (var l in _lines)
            {
                float px = l.X + _tx * 30 * l.Z * _s, py = l.Y + _ty * 20 * l.Z * _s;
                float x2 = px + Cos14 * l.Len, y2 = py + Sin14 * l.Len;
                if (px > W + 4 || x2 < -4 || Math.Max(py, y2) < -4 || Math.Min(py, y2) > H + 4) continue;
                Color c = l.Red ? Theme.Accent : Color.White;
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

        // The hero's five slashes: they sweep in from the left on the first frames, then a
        // soft glint runs along each one every 4.5 s.
        static readonly (float Top, float H, float Delay, float Op, bool Red)[] Slashes =
        {
            (0.30f, 3, 0.15f, 1f, false), (0.34f, 18, 0.30f, 0.95f, false), (0.66f, 6, 0.45f, 1f, true),
            (0.71f, 1, 0.60f, 0.7f, false), (0.12f, 1, 0.75f, 0.4f, false),
        };

        Bitmap _slashCache;
        const int SlashMargin = 40;

        // Once the slashes have swept in they only move with the pointer, so they are drawn
        // into a bitmap (with room for the parallax) and only the glint is drawn per frame.
        void DrawSlashes(Graphics g, int W, int H, float t)
        {
            bool settled = t > Slashes.Max(sl => sl.Delay) + 0.95f;
            int m = (int)(SlashMargin * _s);
            if (settled && (_slashCache == null || _slashCache.Width != W + 2 * m || _slashCache.Height != H + 2 * m))
            {
                _slashCache?.Dispose();
                _slashCache = new Bitmap(W + 2 * m, H + 2 * m, PixelFormat.Format32bppPArgb);
                using var cg = Graphics.FromImage(_slashCache);
                cg.SmoothingMode = SmoothingMode.AntiAlias;
                cg.PixelOffsetMode = PixelOffsetMode.HighQuality;
                cg.TranslateTransform(m, m);
                DrawSlashBars(cg, W, H, 0, 0, t, glint: false);
            }
            if (settled)
            {
                g.DrawImageUnscaled(_slashCache, (int)Math.Round(_tx * 24 * _s) - m, (int)Math.Round(_ty * 14 * _s) - m);
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
                float e = Theme.Ease(Clamp01((t - sl.Delay) / 0.9f));
                if (e <= 0) continue;
                float y = y0 + sl.Top * ch, h = Math.Max(1, sl.H * _s), w = full * e;
                Color c = sl.Red ? Theme.Accent : Theme.Text;

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

        // ---------------------------------------------------------------------------
        // Paint
        void OnPaint(object sender, PaintEventArgs e)
        {
            var g = e.Graphics;
            int W = ClientSize.Width, H = ClientSize.Height;
            if (W <= 0 || H <= 0) return;
            float s = H / 720f;
            if (s != _s) { _s = s; ClearScaleCaches(); }

            _hits.Clear();
            _hovOn.Clear();
            if (_view != _paintedView)
            {
                bool homeChanged = (_view == "home") != (_paintedView == "home");
                _paintedView = _view;
                _viewAt = Now;
                if (_view == "home") _heroAt = Now;
                if (_view == "setup_modding") _emus = FindEmulators();
                if (homeChanged) { _bg?.Dispose(); _bg = null; }
            }
            if (_showSetupPrompt != _paintedPrompt) { _paintedPrompt = _showSetupPrompt; _promptAt = Now; }
            if (_pluginPrompt != _paintedPluginPrompt) { _paintedPluginPrompt = _pluginPrompt; _pluginPromptAt = Now; }
            if (_openDropdownKey != _paintedDd) { _paintedDd = _openDropdownKey; _ddAt = Now; _ddFirst = -1; }

            g.TextRenderingHint = TextRenderingHint.AntiAlias;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;

            bool home = _view == "home";
            if (_bg == null || _bg.Width != W || _bg.Height != H)
            {
                _bg?.Dispose();
                using var raw = Theme.MakeBackground(W, H, null, slashes: !home);
                _bg = raw.Clone(new Rectangle(0, 0, W, H), PixelFormat.Format32bppPArgb);
            }
            g.DrawImageUnscaled(_bg, 0, 0);

            EnsureField(W, H);
            DrawField(g, W, H, home ? 1f : 0.55f);
            if (home) DrawSlashes(g, W, H, _shownAt < 0 ? 0 : (float)(Now - _shownAt));

            switch (_view)
            {
                case "mods": PaintMods(g, W, H); break;
                case "mods_browser": PaintModsBrowser(g, W, H); break;
                case "setup_modding": PaintSetupModding(g, W, H); break;
                case "settings": PaintSettings(g, W, H); break;
                default: PaintHome(g, W, H); break;
            }

            PaintHeader(g, W, H);
            if (_loadOn && _view != "mods") PaintLoad(g, W, H);
            PaintStatus(g, W, H);
            if (_view == "mods" && _showSetupPrompt) PaintSetupPrompt(g, W, H);
            if (_shaderPrompt) PaintShaderPrompt(g, W, H);
            if (_pluginPrompt) PaintPluginPrompt(g, W, H);
        }

        // ---------------------------------------------------------------------------
        // Header: brand, section links, the PLAY call to action, window buttons
        void PaintHeader(Graphics g, int W, int H)
        {
            float s = _s;
            float e = _shownAt < 0 ? 0 : Up((float)(Now - _shownAt), 0, 0.6f);
            var st = PushFade(g, e, -10 * s);

            if (_view != "home")
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s)))
                    g.DrawLine(pen, 0, HeaderH * s, W, HeaderH * s);

            // Brand: the emblem tilts on hover (.brand:hover .brand-mark)
            float bh = Theme.Ease(Hov("brand", _hover == "brand"));
            float mark = 34 * s, mx = 40 * s, my = (HeaderH * s - mark) / 2;
            if (_logo != null)
            {
                var ms = g.Save();
                g.TranslateTransform(mx + mark / 2, my + mark / 2);
                g.RotateTransform(-8 * bh);
                g.ScaleTransform(1 + 0.08f * bh, 1 + 0.08f * bh);
                using var ia = new ImageAttributes();
                ia.SetColorMatrix(new ColorMatrix { Matrix33 = _alpha });
                g.DrawImage(_logo, new Rectangle((int)(-mark / 2), (int)(-mark / 2), (int)mark, (int)mark), 0, 0, _logo.Width, _logo.Height, GraphicsUnit.Pixel, ia);
                g.Restore(ms);
            }
            float wx = mx + mark + 10 * s;
            float ww = Theme.Label(g, "OPENSMASH", FD(21 * s), A(Theme.Text), wx, 0, HeaderH * s, 0.84f * s);
            float ww2 = Theme.Label(g, "-ULTIMATE", FD(21 * s), A(Theme.Dim), wx + ww + 0.84f * s, 0, HeaderH * s, 0.84f * s);
            AddHit("brand", mx, my, mark + 10 * s + ww + ww2, mark);

            // Window buttons
            float cx = W - 40 * s, cy = HeaderH * s / 2, a = 7 * s;
            float ch = Hov("close", _hover == "close");
            using (var pen = new Pen(A(Theme.Mix(Theme.Muted, Theme.Accent, ch)), Math.Max(1.4f, 1.6f * s)))
            {
                g.DrawLine(pen, cx - a, cy - a, cx + a, cy + a);
                g.DrawLine(pen, cx + a, cy - a, cx - a, cy + a);
            }
            AddHit("close", cx - 17 * s, cy - 17 * s, 34 * s, 34 * s);
            float nx = cx - 40 * s, mh = Hov("min", _hover == "min");
            using (var pen = new Pen(A(Theme.Mix(Theme.Muted, Theme.Text, mh)), Math.Max(1.4f, 1.6f * s)))
                g.DrawLine(pen, nx - a, cy, nx + a, cy);
            AddHit("min", nx - 17 * s, cy - 17 * s, 34 * s, 34 * s);

            // Call to action, with arrows for Play (mods) and Vanilla
            var cf = FB(13 * s, true);
            float csp = 13 * s * 0.12f;
            float labelW = Math.Max(Theme.TextWidth(g, "PLAY", cf, csp), Theme.TextWidth(g, "VANILLA", cf, csp));
            float cw = labelW + 44 * s, chh = 38 * s, gap = 6 * s, aw = chh;
            float ctaX = nx - 30 * s - cw;
            float prevX = ctaX - gap - aw, nextX = ctaX + cw + gap;
            string playLabel = _launchMode == "vanilla" ? "VANILLA" : "PLAY";
            Theme.ButtonAnimated(g, "<", prevX, cy - chh / 2, aw, chh, s, primary: false, Hov("nav:mode-prev", _hover == "nav:mode-prev"), _built, 16 * s, _alpha);
            Theme.ButtonAnimated(g, playLabel, ctaX, cy - chh / 2, cw, chh, s, primary: true, Hov("nav:play", _hover == "nav:play"), _built, 13 * s, _alpha);
            Theme.ButtonAnimated(g, ">", nextX, cy - chh / 2, aw, chh, s, primary: false, Hov("nav:mode-next", _hover == "nav:mode-next"), _built, 16 * s, _alpha);
            AddHit("nav:mode-prev", prevX, cy - chh / 2, aw, chh);
            AddHit("nav:play", ctaX, cy - chh / 2, cw, chh);
            AddHit("nav:mode-next", nextX, cy - chh / 2, aw, chh);

            // Section links with the red skewed underline
            string active = _view == "home" ? "nav:home" : _view == "settings" ? "nav:settings" : "nav:mods";
            float lx = prevX - 8 * s;
            foreach (var (id, label) in new[] { ("nav:settings", "SETTINGS"), ("nav:mods", "MODS"), ("nav:home", "HOME") })
            {
                float tw = Theme.TextWidth(g, label, cf, csp), bw = tw + 24 * s;
                lx -= bw;
                bool on = id == active;
                float hv = Hov(id, _hover == id), u = on ? 1 : Theme.Ease(hv);
                Theme.Label(g, label, cf, A(on ? Theme.Text : Theme.Mix(Theme.Muted, Theme.Text, hv)), lx + 12 * s, 0, HeaderH * s, csp);
                if (u > 0)
                {
                    float uy = cy + 13 * s, uw = tw * u, k = 2 * s * 0.577f;
                    using var b = new SolidBrush(A(Theme.Accent));
                    g.FillPolygon(b, new[] { new PointF(lx + 12 * s + k, uy), new PointF(lx + 12 * s + uw + k, uy), new PointF(lx + 12 * s + uw, uy + 2 * s), new PointF(lx + 12 * s, uy + 2 * s) });
                }
                AddHit(id, lx, cy - 18 * s, bw, 36 * s);
            }

            PopFade(g, st);
        }

        // ---------------------------------------------------------------------------
        // Status strip along the bottom: build state, counts, keyboard hints
        void PaintStatus(Graphics g, int W, int H)
        {
            float s = _s, y = H - StatusH * s, h = StatusH * s;
            float e = _shownAt < 0 ? 0 : Up((float)(Now - _shownAt), 0.3f, 0.6f);
            var st = PushFade(g, e, 10 * s);

            using (var pen = new Pen(A(Theme.Line), Math.Max(1, s)))
                g.DrawLine(pen, 0, y, W, y);

            if (_shaderUi)
            {
                PaintJobStrip(g, W, y, h, s,
                    string.IsNullOrEmpty(_shaderDetail) ? "Preparing shaders" : _shaderDetail,
                    _shaderFrac, _shaderEta);
                PopFade(g, st);
                return;
            }
            if (_pluginBuildUi)
            {
                PaintJobStrip(g, W, y, h, s,
                    string.IsNullOrEmpty(_pluginDetail) ? "Starting the plugin build" : _pluginDetail,
                    _pluginFrac, _pluginEta);
                PopFade(g, st);
                return;
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
            Color dc = _built ? Theme.Ready : Theme.Bad;
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
            float lw = Theme.Label(g, _built ? "READY" : "NOT BUILT", fb, A(Theme.Text), x + dot + 10 * s, y, h, sp);
            string info = _built
                ? $"GAME {(_version != "" ? _version : "?")}  /  {_mods.Count} MOD{(_mods.Count == 1 ? "" : "S")}  /  {_plugins} PLUGIN{(_plugins == 1 ? "" : "S")}"
                : "RUN THE INSTALLER TO BUILD THE GAME";
            Theme.Label(g, info, FM(11 * s), A(Theme.Dim), x + dot + 10 * s + lw + 18 * s, y, h, sp);

            string hint = _view switch
            {
                "home" => "\u2190 \u2192  SELECT     ENTER  CONFIRM",
                "mods" => "\u2191 \u2193  SELECT     DEL  REMOVE     ESC  BACK",
                "settings" => "\u2190 \u2192  TABS     ENTER  CHANGE     ESC  BACK",
                _ => "ESC  BACK",
            };
            Theme.Label(g, hint, FM(11 * s), A(Theme.Dim), 0, y, h, sp, StringAlignment.Far, W - 40 * s);

            PopFade(g, st);
        }

        // Full-width bar in the status strip: the step on the left, percent and time left on the right.
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
                    using var fill = new SolidBrush(A(Theme.Accent));
                    g.FillRectangle(fill, c0, y, c1 - c0, barH);
                }
            }
            else
            {
                using var fill = new SolidBrush(A(Theme.Accent));
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
            Theme.Label(g, step, font, A(Theme.Text), pad, textTop, textH, sp);
            if (right.Length > 0)
                Theme.Label(g, right, font, A(Theme.Muted), 0, textTop, textH, sp, StringAlignment.Far, W - pad);
        }

        // The open strip under the home chips: how far Play has got before the game window is up.
        void PaintLoad(Graphics g, int W, int H)
        {
            float s = _s;
            float x = 104 * s;
            float w = Math.Min(640 * s, W - x - 80 * s);
            float labelY = 618 * s;
            float barY = 644 * s;
            float barH = 8 * s;
            float statusTop = H - StatusH * s;
            if (barY + barH > statusTop - 10 * s)
            {
                barY = statusTop - 18 * s;
                labelY = barY - 24 * s;
            }
            // The mods page keeps its buttons in this band, so the bar sits above them.
            float occupied = statusTop - 12 * s;
            if (_view == "mods") occupied = Math.Min(occupied, H - 104 * s - 14 * s);
            if (barY + barH > occupied)
            {
                barY = occupied - barH;
                labelY = barY - 24 * s;
            }
            string label = string.IsNullOrEmpty(_loadLabel) ? "STARTING THE GAME" : _loadLabel;
            Theme.Label(g, label, FM(12 * s), A(Theme.Muted), x, labelY, 18 * s, 12 * s * 0.08f);

            float cut = 5 * s;
            using (var track = Theme.Para(x, barY, w, barH, cut))
            using (var b = new SolidBrush(A(White(16))))
                g.FillPath(b, track);

            float p0, p1;
            if (_loadProgress < 0)
            {
                float span = 0.28f;
                float t = (float)(Now % 1.3 / 1.3);
                p0 = t * (1 + span) - span;
                p1 = p0 + span;
            }
            else
            {
                p0 = 0;
                p1 = Math.Clamp(_loadProgress, 0.02f, 1f);
            }
            float x0 = x + Math.Max(0, p0) * w;
            float x1 = x + Math.Min(1, p1) * w;
            if (x1 - x0 > 1)
            {
                var st = g.Save();
                using (var clip = Theme.Para(x, barY, w, barH, cut))
                    g.SetClip(clip);
                using (var fill = Theme.Para(x0, barY, Math.Max(barH, x1 - x0), barH, cut))
                using (var b = new SolidBrush(A(Theme.Accent)))
                    g.FillPath(b, fill);
                g.Restore(st);
            }
        }

        // ---------------------------------------------------------------------------
        // Home: the site's hero
        void PaintHome(Graphics g, int W, int H)
        {
            float s = _s, t = (float)(Now - _heroAt), left = 104 * s;

            // .hero-vert
            {
                var st = PushFade(g, _shownAt < 0 ? 0 : Up((float)(Now - _shownAt), 0.2f), 0);
                var vs = g.Save();
                g.TranslateTransform(40 * s, H / 2f);
                g.RotateTransform(-90);
                Theme.Label(g, "NATIVE / PC / OPEN SOURCE", FM(12 * s), A(Theme.Dim), -300 * s, -8 * s, 16 * s, 6 * s, StringAlignment.Center, 600 * s);
                g.Restore(vs);
                PopFade(g, st);
            }

            // .kicker
            {
                var st = PushFade(g, Up(t, 0.2f), 24 * s);
                float ky = 128 * s, kh = 16 * s;
                bool lit = Now % 1.6 < 0.8;
                using (var b = new SolidBrush(A(Color.FromArgb(lit ? 255 : 51, Theme.Accent))))
                    g.FillPolygon(b, new[] { new PointF(left + 3 * s, ky + 4 * s), new PointF(left + 11 * s, ky + 4 * s), new PointF(left + 8 * s, ky + 12 * s), new PointF(left, ky + 12 * s) });
                string kicker = $"NATIVE RECOMPILATION  \u00B7  GAME {(_version != "" ? _version : "?")}  \u00B7  WINDOWS X64";
                Theme.Label(g, kicker, FM(13 * s), A(Theme.Muted), left + 21 * s, ky, kh, 13 * s * 0.14f);
                PopFade(g, st);
            }

            // .hero-title: each word rises out of its line with a skew
            float px = 150 * s, capTop = 160 * s, step = 0.86f * px;
            float openW = DrawHeroWord(g, "OPEN", left, capTop, px, t, 0.35f, Theme.Text, false);
            DrawHeroWord(g, "SMASH", left + openW, capTop, px, t, 0.47f, Theme.Accent, false);
            DrawHeroWord(g, "ULTIMATE", left + 110 * s, capTop + step, px, t, 0.60f, Theme.Text, true);

            // .tagline
            {
                var st = PushFade(g, Up(t, 0.9f), 24 * s);
                DrawRich(g, new[]
                {
                    ("Recompiled, not emulated. ", FB(19 * s, true), Theme.Text),
                    ("Super Smash Bros. Ultimate as a real Windows program, built on this PC from your own copy.", FB(19 * s), Theme.Muted),
                }, left, 420 * s, 600 * s, 30 * s);
                PopFade(g, st);
            }

            // .cta-row: < PLAY > (same mode as the header), MODS, SETTINGS, QUIT
            {
                var st = PushFade(g, Up(t, 1.05f), 24 * s);
                float bx = left, by = 494 * s, bh = 52 * s, aw = bh, ag = 8 * s;
                float[] widths = { 220, 150, 184, 128 };
                string playLabel = _launchMode == "vanilla" ? "VANILLA" : "PLAY";
                Theme.ButtonAnimated(g, "<", bx, by, aw, bh, s, primary: false, Hov("nav:mode-prev", _hover == "nav:mode-prev"), _built, 18 * s, _alpha);
                AddHit("nav:mode-prev", bx, by, aw, bh);
                bx += aw + ag;
                for (int i = 0; i < _menuItems.Length; i++)
                {
                    string id = $"menu:{i}";
                    float bw = widths[i] * s;
                    float hv = Hov(id, _hover == id || (_keys && _sel == i));
                    Theme.ButtonAnimated(g, i == 0 ? playLabel : _menuItems[i], bx, by, bw, bh, s, primary: i == 0, hv, i != 0 || _built, 15 * s, _alpha,
                        i == 0 ? DrawArrow : null);
                    AddHit(id, bx, by, bw, bh);
                    bx += bw;
                    if (i == 0)
                    {
                        bx += ag;
                        Theme.ButtonAnimated(g, ">", bx, by, aw, bh, s, primary: false, Hov("nav:mode-next", _hover == "nav:mode-next"), _built, 18 * s, _alpha);
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
                foreach (string chip in new[] { "VULKAN RENDERER", $"{_mods.Count} MOD{(_mods.Count == 1 ? "" : "S")}", $"{_plugins} PLUGIN{(_plugins == 1 ? "" : "S")}", "F1  IN-GAME MENU" })
                {
                    float cw = Theme.TextWidth(g, chip, f, sp) + 26 * s;
                    using (var p = Theme.Para(cx, cy, cw, chh, 6 * s))
                    {
                        using (var b = new SolidBrush(A(White(8)))) g.FillPath(b, p);
                        using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                    }
                    Theme.Label(g, chip, f, A(Theme.Muted), cx, cy, chh, sp, StringAlignment.Center, cw);
                    cx += cw + 8 * s;
                }
                PopFade(g, st);
            }
        }

        // M5 12h12 M12 5l7 7-7 7, the arrow of the site's GET IT button
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
            float e = Theme.Ease(Clamp01((t - delay) / 1f));
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

        // Word-wrapped runs of differently styled text (the tagline's bold lead-in).
        void DrawRich(Graphics g, (string Text, Font Font, Color Color)[] runs, float x, float y, float maxW, float lineH)
        {
            float cx = x, cy = y;
            foreach (var run in runs)
            {
                using var b = new SolidBrush(A(run.Color));
                var (cap, top) = Theme.Caps(run.Font);
                foreach (string word in run.Text.Split(' '))
                {
                    if (word.Length == 0) continue;
                    string piece = word + " ";
                    float w = g.MeasureString(piece, run.Font, 100000, Theme.Typo).Width;
                    if (cx + w - g.MeasureString(" ", run.Font, 100000, Theme.Typo).Width > x + maxW && cx > x) { cx = x; cy += lineH; }
                    g.DrawString(piece, run.Font, b, cx, cy + (lineH - cap) / 2 - top, Theme.Typo);
                    cx += w;
                }
            }
        }

        // ---------------------------------------------------------------------------
        // Section heading: "02 / MODS" eyebrow and the big title, which slides in skewed
        // under a white bar that wipes across it (.big.reveal)
        void PaintSectionHead(Graphics g, string eyebrow, string title)
        {
            float s = _s, t = (float)(Now - _viewAt), x = Gutter * s;

            {
                var st = PushFade(g, Up(t, 0), 24 * s);
                float ey = 86 * s, eh = 16 * s, k = 2 * s * 0.577f;
                float bw = 96 * s, bh = 32 * s, by = (HeaderH + 6) * s;
                Theme.ButtonAnimated(g, "BACK", x, by, bw, bh, s, primary: false, Hov("nav:back", _hover == "nav:back"), true, 12 * s, _alpha);
                AddHit("nav:back", x, by, bw, bh);
                float ex = x + bw + 18 * s;
                using (var b = new SolidBrush(A(Theme.Accent)))
                    g.FillPolygon(b, new[] { new PointF(ex + k, ey + eh / 2 - s), new PointF(ex + 34 * s + k, ey + eh / 2 - s), new PointF(ex + 34 * s, ey + eh / 2 + s), new PointF(ex, ey + eh / 2 + s) });
                Theme.Label(g, eyebrow, FM(13 * s), A(Theme.Accent), ex + 46 * s, ey, eh, 13 * s * 0.2f);
                PopFade(g, st);
            }

            float px = 66 * s, capTop = SectionTitleTop * s;
            var path = TextPath(title, px);
            var dot = TextPath(".", px);
            var b0 = path.GetBounds();
            float adv = Advance(g, title, px), full = adv + Advance(g, ".", px);

            float tr = Theme.Ease(Clamp01((t - 0.2f) / 0.9f));
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
                using (var b = new SolidBrush(Theme.Fade(Theme.Text, op))) g.FillPath(b, path);
                var ds = g.Save();
                g.TranslateTransform(adv, 0);
                using (var b = new SolidBrush(Theme.Fade(Theme.Accent, op))) g.FillPath(b, dot);
                g.Restore(ds);
            }

            float wt = t / 0.85f;
            if (wt < 1)
            {
                float bl = -0.02f * full, bw = 1.04f * full, wl, wr;
                if (wt <= 0.45f) { wl = bl; wr = bl + bw * Theme.Ease(wt / 0.45f); }
                else { wl = bl + bw * Theme.Ease(Clamp01((wt - 0.46f) / 0.54f)); wr = bl + bw; }
                float top = boxTop - capTop + b0.Top, hgt = boxH;
                using var b = new SolidBrush(Theme.Text);
                g.FillRectangle(b, wl, top + 0.04f * hgt, Math.Max(0, wr - wl), hgt * 0.92f);
            }
            g.Restore(st2);
        }

        // ---------------------------------------------------------------------------
        // Small parts: badges, cards, list rows, fields
        float Badge(Graphics g, string text, float x, float y, Color bg, Color fg, bool outline = false)
        {
            var f = FM(11 * _s, true);
            float sp = 11 * _s * 0.12f, h = 20 * _s;
            float w = Theme.TextWidth(g, text, f, sp) + 20 * _s;
            using (var p = Theme.Para(x, y, w, h, 5 * _s))
            {
                if (outline) using (var pen = new Pen(A(Theme.Dim), Math.Max(1, _s))) g.DrawPath(pen, p);
                else using (var b = new SolidBrush(A(bg))) g.FillPath(b, p);
            }
            Theme.Label(g, text, f, A(fg), x, y, h, sp, StringAlignment.Center, w);
            return w;
        }

        void PaintCard(Graphics g, string id, float x, float y, float w, float h, int reveal, bool keyHot,
            string badge, Color badgeBg, Color badgeFg, string title, string desc)
        {
            float s = _s;
            var st = PushReveal(g, reveal);
            bool live = id != "";
            float hv = live ? Hov(id, _hover == id || keyHot) : 0, e = Theme.Ease(hv);
            y -= 6 * s * e;
            float cut = 22 * s;
            using var path = Theme.CutCorner(x, y, w, h, cut);
            using (var b = new SolidBrush(A(Theme.Mix(Theme.Bg2, Theme.Bg3, e)))) g.FillPath(b, path);

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

            using (var pen = new Pen(A(Theme.Mix(Theme.Line, White(102), e)), Math.Max(1, s))) g.DrawPath(pen, path);
            using (var pen = new Pen(A(Theme.Dim), Math.Max(1, 1.5f * s))) g.DrawLine(pen, x + w - cut, y, x + w, y + cut);

            float pad = 24 * s;
            Badge(g, badge, x + pad, y + 20 * s, badgeBg, badgeFg, badgeBg == Color.Empty);
            Theme.Label(g, title, FD(30 * s), A(Theme.Text), x + pad, y + 52 * s, 26 * s, 0.3f * s);
            Theme.DrawText(g, desc, FB(14 * s), A(Theme.Muted), x + pad, y + 88 * s, w - pad * 2 - 40 * s, 20 * s);
            if (live)
            {
                float ax = x + w - 44 * s + 6 * s * e;
                DrawArrow(g, A(Theme.Mix(Theme.Dim, Theme.Accent, e)), ax, y + h / 2 + 8 * s);
                AddHit(id, x, y, w, h);
            }
            PopFade(g, st);
        }

        // .road-item hover: a red wash that grows from the left
        void RowWash(Graphics g, float x, float y, float w, float h, float e, float strength = 0.14f)
        {
            if (e <= 0) return;
            float ww = Math.Max(1, w * e);
            using var lg = new LinearGradientBrush(new RectangleF(x - 1, y, ww + 2, h), A(Color.FromArgb((int)(255 * strength), Theme.Accent)), Color.FromArgb(0, Theme.Accent), LinearGradientMode.Horizontal);
            g.FillRectangle(lg, x, y, ww, h);
        }

        void HLine(Graphics g, float x1, float x2, float y)
        {
            using var pen = new Pen(A(Theme.Line), Math.Max(1, _s));
            g.DrawLine(pen, x1, y, x2, y);
        }

        // ---------------------------------------------------------------------------
        // Mods
        int ModActionCount => 2;
        string ModActionId(int index) => index == 0 ? "act:browse" : "act:setup";
        float ModListTop => 368 * _s;
        float ModListView => Math.Max(68f, (ClientSize.Height - 118 * _s - ModListTop) / _s);

        void EnsureModVisible()
        {
            if (_sel < ModActionCount) return;
            int row = _sel - ModActionCount;
            if (row < 0 || row >= _modLines.Count) return;
            float y = 0;
            for (int i = 0; i < row; i++) y += _modLines[i].RowH;
            float view = ModListView;
            if (y < _modScroll) _modScroll = (int)y;
            else if (y + _modLines[row].RowH > _modScroll + view)
                _modScroll = (int)Math.Ceiling(y + _modLines[row].RowH - view);
        }

        void PaintMods(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "02 / MODS", "MODS");

            float cardY = ContentTop * s, cardH = 128 * s, gap = 16 * s;
            int cards = ModActionCount;
            float cardW = (cw - gap * (cards - 1)) / cards;
            PaintCard(g, "act:browse", x, cardY, cardW, cardH, 0, _keys && _sel == 0,
                "GAMEBANANA", Theme.Text, Theme.Bg, "BROWSE MODS", "Find and install mods from GameBanana.");
            PaintCard(g, "act:setup", x + (cardW + gap), cardY, cardW, cardH, 1, _keys && _sel == 1,
                "PLUGINS", Theme.Text, Theme.Bg, "SETUP MODDING", "Community plugins movesets and new characters are built on.");

            // List. _modScroll is a pixel offset in unscaled units; rows grow when they carry a description.
            float top = ModListTop;
            float view = ModListView;
            float content = 0;
            foreach (var line in _modLines) content += line.RowH;
            int maxScroll = (int)Math.Max(0, content - view);
            if (_modScroll > maxScroll) _modScroll = maxScroll;
            float sizeR = x + cw - 128 * s, typeX = x + cw - 96 * s;
            {
                var st = PushReveal(g, 2);
                var hf = FM(11 * s);
                float sp = 11 * s * 0.16f;
                int packages = _mods.Select(m => string.IsNullOrEmpty(m.Group) ? "m:" + m.Name : "g:" + m.Group).Distinct().Count();
                Theme.Label(g, $"INSTALLED  ({packages})", hf, A(Theme.Dim), x, top - 26 * s, 20 * s, sp);
                Theme.Label(g, "FILES", hf, A(Theme.Dim), 0, top - 26 * s, 20 * s, sp, StringAlignment.Far, sizeR);
                Theme.Label(g, "ON", hf, A(Theme.Dim), typeX, top - 26 * s, 20 * s, sp);
                HLine(g, x, x + cw, top);
                PopFade(g, st);
            }

            if (_mods.Count == 0 && _modsChecked)
            {
                var st = PushReveal(g, 3);
                var path = TextPath("NO MODS YET.", 46 * s);
                var b = path.GetBounds();
                var ps = g.Save();
                g.TranslateTransform(x, top + 34 * s - b.Top);
                using (var pen = new Pen(A(Theme.Dim), Math.Max(1, 1.5f * s))) g.DrawPath(pen, path);
                g.Restore(ps);
                Theme.Label(g, "Browse GameBanana above. A download is installed and turned on by itself.", FB(15 * s), A(Theme.Muted), x, top + 34 * s + b.Height + 14 * s, 22 * s);
                PopFade(g, st);
            }

            float yAcc = 0;
            int drawn = 0;
            float listBottom = top + view * s;
            var listClip = g.Save();
            g.SetClip(new RectangleF(0, top, W, Math.Max(1, listBottom - top)), CombineMode.Intersect);
            for (int i = 0; i < _modLines.Count; i++)
            {
                var m = _modLines[i];
                float rowUnscaled = m.RowH;
                float topUnscaled = yAcc - _modScroll;
                yAcc += rowUnscaled;
                if (topUnscaled + rowUnscaled < 0) continue;
                if (topUnscaled > view) break;
                int rowIndex = ModActionCount + i;
                string id = $"modrow:{rowIndex}";
                float ry = top + topUnscaled * s;
                float rowH = rowUnscaled * s;
                bool sel = rowIndex == _sel;
                var st = PushReveal(g, 3 + drawn);
                drawn++;
                float e = Theme.Ease(Math.Max(Hov(id, _hover == id), sel ? 1 : 0));
                RowWash(g, x, ry, cw, rowH, e, sel ? 0.2f : 0.14f);
                if (sel) using (var b = new SolidBrush(A(Theme.Accent))) g.FillRectangle(b, x, ry, 3 * s, rowH);
                float shift = 10 * s * e;
                float indent = m.Child ? 36 * s : 0;
                float thumb = m.Child ? 0 : 46 * s;
                float tx = x + 14 * s + indent + shift;
                if (m.Header)
                {
                    float cy = ry + rowH / 2, ca = 5 * s;
                    using var pen = new Pen(A(Theme.Muted), Math.Max(1.4f, 1.6f * s));
                    if (m.Open)
                        g.DrawLines(pen, new[] { new PointF(tx, cy - ca * 0.4f), new PointF(tx + ca, cy + ca * 0.45f), new PointF(tx + ca * 2, cy - ca * 0.4f) });
                    else
                        g.DrawLines(pen, new[] { new PointF(tx, cy - ca), new PointF(tx + ca * 0.8f, cy), new PointF(tx, cy + ca) });
                    tx += 22 * s;
                }
                if (!m.Child)
                {
                    float dw = thumb, dh = thumb;
                    if (!string.IsNullOrEmpty(m.Thumb) && _modThumbs.TryGetValue(m.Thumb, out var pic) && pic.Width > 0 && pic.Height > 0)
                    {
                        float maxW = 96 * s;
                        dh = thumb;
                        dw = dh * pic.Width / (float)pic.Height;
                        if (dw > maxW) { dw = maxW; dh = dw * pic.Height / (float)pic.Width; }
                        g.DrawImage(pic, tx, ry + (rowH - dh) / 2, dw, dh);
                    }
                    else
                    {
                        using var b = new SolidBrush(A(Theme.Bg2));
                        g.FillRectangle(b, tx, ry + (rowH - thumb) / 2, thumb, thumb);
                    }
                    tx += dw + 14 * s;
                }
                bool detailed = m.HasChips || !string.IsNullOrEmpty(m.About) || !string.IsNullOrEmpty(m.Meta) || !string.IsNullOrEmpty(m.Sub);
                float textW = Math.Max(40 * s, sizeR - tx - 108 * s);
                if (!detailed)
                    Theme.DrawText(g, m.Title, FB(15 * s, sel), A(m.Enabled || m.Mixed ? Theme.Text : Theme.Dim), tx, ry, textW, rowH);
                else
                {
                    float ty = ry + 8 * s;
                    Theme.DrawText(g, m.Title, FB(15 * s, sel), A(m.Enabled || m.Mixed ? Theme.Text : Theme.Dim), tx, ty, textW, 22 * s, StringAlignment.Near, StringAlignment.Near);
                    ty += 24 * s;
                    if (!string.IsNullOrEmpty(m.Sub))
                    {
                        Theme.Label(g, m.Sub, FM(12 * s), A(Theme.Dim), tx, ty, 16 * s);
                        ty += 18 * s;
                    }
                    if (!string.IsNullOrEmpty(m.Meta) || m.HasChips)
                    {
                        float bx = tx;
                        float limit = tx + textW;
                        if (!string.IsNullOrEmpty(m.Meta))
                        {
                            Theme.Label(g, m.Meta, FM(11 * s), A(Theme.Dim), bx, ty, 20 * s, 0.08f * s);
                            bx += Theme.TextWidth(g, m.Meta, FM(11 * s), 0.08f * s) + 10 * s;
                        }
                        void Chip(string text, Color bg, Color fg, bool outline = false)
                        {
                            if (bx >= limit - 40 * s) return;
                            bx += Badge(g, text, bx, ty, bg, fg, outline) + 6 * s;
                        }
                        var ink = Color.FromArgb(36, 255, 255, 255);
                        if (m.Online == "unsafe") Chip("NOT ONLINE SAFE", Color.FromArgb(56, 255, 59, 48), Theme.Bad);
                        else if (m.Online == "same") Chip("EVERYONE NEEDS IT", Color.FromArgb(48, 255, 194, 26), Theme.Warn);
                        else if (m.Online == "caution") Chip("CHECK BEFORE ONLINE", Color.FromArgb(48, 255, 194, 26), Theme.Warn);
                        else if (m.Online == "safe") Chip("ONLINE SAFE", Color.FromArgb(40, 61, 255, 138), Theme.Ready);
                        if (m.NewSlots) Chip("NEW COSTUME SLOTS", Color.FromArgb(40, 59, 224, 255), Theme.Info);
                        if (m.Code == "waiting") Chip("PLUGIN NOT BUILT", Color.FromArgb(56, 255, 59, 48), Theme.Bad);
                        else if (m.Code == "built") Chip("PLUGIN BUILT IN", Color.FromArgb(40, 61, 255, 138), Theme.Ready);
                        if (m.Added > 0) Chip(m.Added + " NEW FILES", ink, Theme.Muted, true);
                        if (m.Merged > 0) Chip(m.Merged + " PATCHES", ink, Theme.Muted, true);
                        if (m.KeptShared > 0) Chip(m.KeptShared + " ON ALL COSTUMES", ink, Theme.Muted, true);
                        if (m.Skipped > 0) Chip(m.Skipped + " NOT LOADED", Color.FromArgb(56, 255, 59, 48), Theme.Bad);
                        ty += 26 * s;
                    }
                    if (!string.IsNullOrEmpty(m.About))
                        Theme.DrawWrapped(g, m.About, FB(12 * s), A(Theme.Muted), tx, ty, textW, 42 * s);
                }
                if (!m.Header)
                    Theme.Label(g, (m.Files + " FILES").ToUpperInvariant(), FM(12 * s), A(Theme.Dim), 0, ry, rowH, 0, StringAlignment.Far, sizeR);
                string toggle = m.Mixed ? "MIX" : m.Enabled ? "ON" : "OFF";
                float tw = 64 * s, txx = typeX, tyy = ry + (rowH - 24 * s) / 2;
                using (var pth = Theme.Para(txx, tyy, tw, 24 * s, 6 * s))
                {
                    if (m.Enabled) using (var b = new SolidBrush(A(Theme.Text))) g.FillPath(b, pth);
                    using (var pen = new Pen(A(m.Enabled ? Theme.Text : Theme.Line), Math.Max(1, s))) g.DrawPath(pen, pth);
                }
                Theme.Label(g, toggle, FM(11 * s, true), A(m.Enabled ? Theme.Bg : Theme.Muted), txx, tyy, 24 * s, 0, StringAlignment.Center, tw);
                HLine(g, x, x + cw, ry + rowH);
                float hitTop = Math.Max(ry, top), hitH = Math.Min(ry + rowH, listBottom) - hitTop;
                if (hitH > 0)
                {
                    AddHit(id, x, hitTop, typeX - x - 8 * s, hitH);
                    if (m.Header) AddHit("modopen:" + i, x, hitTop, typeX - x - 8 * s, hitH);
                    float toggleTop = Math.Max(tyy, top), toggleH = Math.Min(tyy + 24 * s, listBottom) - toggleTop;
                    if (toggleH > 0) AddHit("modtoggle:" + i, txx, toggleTop, tw + 8 * s, toggleH);
                }
                PopFade(g, st);
            }
            g.Restore(listClip);

            if (maxScroll > 0)
            {
                float trackX = x + cw + 16 * s;
                float trackH = view * s;
                float thumbH = Math.Max(28 * s, trackH * trackH / Math.Max(trackH, content * s));
                float thumbY = top + (trackH - thumbH) * _modScroll / maxScroll;
                _modTrackTop = top;
                _modTrackH = trackH;
                _modThumbH = thumbH;
                _modMaxScroll = maxScroll;
                using (var b = new SolidBrush(White(28))) g.FillRectangle(b, trackX, top, 3 * s, trackH);
                using (var b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, trackX, thumbY, 3 * s, thumbH);
                AddHit("modscroll", trackX - 10 * s, top, 24 * s, trackH);
            }
            else
                _modMaxScroll = 0;

            // Actions
            {
                var st = PushReveal(g, 5);
                float bx = x, by = H - 104 * s, bh = 38 * s;
                var f = FB(12 * s, true);
                foreach (var (key, label) in new[] { ("open", "OPEN FOLDER"), ("add", "ADD MOD"), ("remove", "REMOVE"), ("refresh", "REFRESH") })
                {
                    string id = $"act:{key}";
                    bool enabled = key != "remove" || _sel >= ModActionCount;
                    float bw = Theme.TextWidth(g, label, f, 1.2f * s) + 48 * s;
                    Theme.ButtonAnimated(g, label, bx, by, bw, bh, s, primary: false, Hov(id, _hover == id), enabled, 12 * s, _alpha);
                    AddHit(id, bx, by, bw, bh);
                    bx += bw + 10 * s;
                }
                PopFade(g, st);
            }
        }

        void PaintSetupPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = Theme.Ease(Clamp01((float)(Now - _promptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);

            var st = PushFade(g, e, 24 * s);
            float mw = 540 * s, mh = 262 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = Theme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(Theme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(Theme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            Theme.Label(g, "NO MODS INSTALLED", FD(32 * s), A(Theme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            Theme.DrawWrapped(g, "You don't have any mods yet. Set up modding first? This installs Skyline and ARCropolis, which every mod needs to load.",
                FB(15 * s), A(Theme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 70 * s);

            float bh = 46 * s, by = my + mh - bh - 30 * s;
            float w1 = 210 * s, w2 = 150 * s;
            float x1 = mx + mw - pad - w1, x2 = x1 - 12 * s - w2;
            Theme.ButtonAnimated(g, "CANCEL", x2, by, w2, bh, s, false, Hov("setup_prompt:cancel", _hover == "setup_prompt:cancel"), true, 13 * s, _alpha);
            AddHit("setup_prompt:cancel", x2, by, w2, bh);
            Theme.ButtonAnimated(g, "SETUP MODDING", x1, by, w1, bh, s, true, Hov("setup_prompt:setup", _hover == "setup_prompt:setup"), true, 13 * s, _alpha);
            AddHit("setup_prompt:setup", x1, by, w1, bh);
            PopFade(g, st);
        }

        void PaintShaderPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = Theme.Ease(Clamp01((float)(Now - _shaderPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("shader_prompt:block", 0, 0, W, H);

            var st = PushFade(g, e, 24 * s);
            float mw = 760 * s, mh = 300 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = Theme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(Theme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(Theme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            Theme.Label(g, "FINISH SHADERS", FD(32 * s), A(Theme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            Theme.DrawWrapped(g, "Shader compile has not finished, so a new effect can stutter the first time you see it. Pick how hard this PC should work. The bar along the bottom shows the progress. You can keep playing while it runs.",
                FB(15 * s), A(Theme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 90 * s);

            float bh = 46 * s, by = my + mh - bh - 28 * s, gap = 10 * s;
            float wLater = 120 * s, wPri = 140 * s;
            float xHigh = mx + mw - pad - wPri;
            float xMed = xHigh - gap - wPri;
            float xLow = xMed - gap - wPri;
            float xLater = xLow - gap - wLater;
            Theme.ButtonAnimated(g, "LATER", xLater, by, wLater, bh, s, false, Hov("shader_prompt:later", _hover == "shader_prompt:later"), true, 13 * s, _alpha);
            AddHit("shader_prompt:later", xLater, by, wLater, bh);
            Theme.ButtonAnimated(g, "LOW", xLow, by, wPri, bh, s, false, Hov("shader_prompt:low", _hover == "shader_prompt:low"), true, 13 * s, _alpha);
            AddHit("shader_prompt:low", xLow, by, wPri, bh);
            Theme.ButtonAnimated(g, "MEDIUM", xMed, by, wPri, bh, s, false, Hov("shader_prompt:medium", _hover == "shader_prompt:medium"), true, 13 * s, _alpha);
            AddHit("shader_prompt:medium", xMed, by, wPri, bh);
            Theme.ButtonAnimated(g, "HIGH", xHigh, by, wPri, bh, s, true, Hov("shader_prompt:high", _hover == "shader_prompt:high"), true, 13 * s, _alpha);
            AddHit("shader_prompt:high", xHigh, by, wPri, bh);
            PopFade(g, st);
        }

        void PaintPluginPrompt(Graphics g, int W, int H)
        {
            float s = _s, e = Theme.Ease(Clamp01((float)(Now - _pluginPromptAt) / 0.4f));
            using (var b = new SolidBrush(Color.FromArgb((int)(200 * e), 5, 5, 5))) g.FillRectangle(b, 0, 0, W, H);
            AddHit("plugin_prompt:block", 0, 0, W, H);

            var st = PushFade(g, e, 24 * s);
            float mw = 640 * s, mh = 320 * s, mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = Theme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(A(Theme.Bg2))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(A(Theme.Accent))) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            float pad = 32 * s;
            Theme.Label(g, "UPDATE PLUGINS", FD(32 * s), A(Theme.Text), mx + pad, my + 34 * s, 30 * s, 0.32f * s);
            Theme.DrawWrapped(g, _pluginPromptText, FB(15 * s), A(Theme.Muted), mx + pad, my + 84 * s, mw - pad * 2, 130 * s);

            float bh = 46 * s, by = my + mh - bh - 28 * s, gap = 12 * s;
            float wBuild = 230 * s, wPlay = 160 * s, wCancel = 120 * s;
            float xBuild = mx + mw - pad - wBuild;
            float xPlay = xBuild - gap - wPlay;
            float xCancel = xPlay - gap - wCancel;
            Theme.ButtonAnimated(g, "CANCEL", xCancel, by, wCancel, bh, s, false, Hov("plugin_prompt:cancel", _hover == "plugin_prompt:cancel"), true, 13 * s, _alpha);
            AddHit("plugin_prompt:cancel", xCancel, by, wCancel, bh);
            Theme.ButtonAnimated(g, "PLAY ANYWAY", xPlay, by, wPlay, bh, s, false, Hov("plugin_prompt:play", _hover == "plugin_prompt:play"), true, 13 * s, _alpha);
            AddHit("plugin_prompt:play", xPlay, by, wPlay, bh);
            Theme.ButtonAnimated(g, "UPDATE PLUGINS", xBuild, by, wBuild, bh, s, true, Hov("plugin_prompt:build", _hover == "plugin_prompt:build"), true, 13 * s, _alpha);
            AddHit("plugin_prompt:build", xBuild, by, wBuild, bh);
            PopFade(g, st);
        }

        // ---------------------------------------------------------------------------
        // Mods browser (WebView2 marketplace below the heading)
        void PaintModsBrowser(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s;
            PaintSectionHead(g, "02 / MODS", "BROWSE MODS");
            {
                var st = PushFade(g, Up((float)(Now - _viewAt), 0.4f), 16 * s);
                Theme.Label(g, "POWERED BY GAMEBANANA", FM(12 * s), A(Theme.Dim), 0, SectionTitleTop * s + 14 * s, 20 * s, 12 * s * 0.16f, StringAlignment.Far, W - x);
                PopFade(g, st);
            }

            if (_marketplaceWebView != null)
            {
                int y = (int)Math.Round(ContentTop * s - 8 * s);
                int height = H - y - (int)Math.Round(StatusH * s) - 1;
                var bounds = new Rectangle(0, y, Math.Max(1, W), Math.Max(1, height));
                if (_marketplaceWebView.Bounds != bounds) _marketplaceWebView.Bounds = bounds;
                if (!_marketplaceWebView.Visible) _marketplaceWebView.Visible = true;
                _marketplaceWebView.BringToFront();

                // The page owns everything below the heading; keep only the header's hit regions.
                _hits.RemoveAll(h => h.Id != "close" && h.Id != "min" && h.Id != "brand" && !h.Id.StartsWith("nav:"));
            }
            else if (!string.IsNullOrEmpty(_marketplaceError))
            {
                Theme.DrawText(g, _marketplaceError, FB(15 * s), Theme.Bad, x, H / 2 - 40 * s, W - 2 * x, 90 * s,
                    StringAlignment.Center, StringAlignment.Center);
            }
        }

        // ---------------------------------------------------------------------------
        // Setup modding
        void PaintSetupModding(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "02 / MODS", "SETUP MODDING");

            {
                var st = PushReveal(g, 0);
                Theme.DrawWrapped(g, "The community plugins movesets and new characters are built on. Downloading them here is the same list WindowSmash Mods keeps under Settings.",
                    FB(16 * s), A(Theme.Muted), x, ContentTop * s - 4 * s, cw, 52 * s);
                PopFade(g, st);
            }

            var items = _stdPlugins.Where(p => p.Status == "runs").ToArray();
            float listY = ContentTop * s + 62 * s, rowH = 40 * s;
            {
                var st = PushReveal(g, 1);
                HLine(g, x, x + cw, listY);
                for (int i = 0; i < items.Length; i++)
                {
                    float ry = listY + i * rowH;
                    string label = items[i].Name.Replace(".nro", "") + "   " + items[i].Version;
                    if (items[i].Fetched) Badge(g, "FETCHED", x, ry + (rowH - 20 * s) / 2, Theme.Text, Theme.Bg);
                    else Badge(g, "MISSING", x, ry + (rowH - 20 * s) / 2, Color.Empty, Theme.Muted, outline: true);
                    Theme.Label(g, label, FB(15 * s), A(items[i].Fetched ? Theme.Text : Theme.Muted), x + 104 * s, ry, rowH);
                    HLine(g, x, x + cw, ry + rowH);
                }
                PopFade(g, st);
            }

            float barY = listY + items.Length * rowH + 34 * s;
            {
                var st = PushReveal(g, 2);
                var f = FM(12 * s);
                Theme.Label(g, string.IsNullOrEmpty(_setupStep) ? "PROGRESS" : _setupStep.ToUpperInvariant(), f, A(Theme.Muted), x, barY - 24 * s, 18 * s, 1.4f * s);
                Theme.Label(g, $"{_setupProgress}%", FM(12 * s, true), A(Theme.Text), 0, barY - 24 * s, 18 * s, 1.4f * s, StringAlignment.Far, x + cw);
                DrawBar(g, x, barY, cw, 8 * s, _setupProgress / 100f, Theme.Accent);
                PopFade(g, st);
            }

            float logY = barY + 26 * s, logH = H - 124 * s - logY;
            if (logH > 30 * s)
            {
                var st = PushReveal(g, 3);
                using (var b = new SolidBrush(A(Theme.Bg2))) g.FillRectangle(b, x, logY, cw, logH);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawRectangle(pen, x, logY, cw, logH);
                float lh = 16 * s;
                var lines = _setupLog.Split('\n').Where(l => !string.IsNullOrWhiteSpace(l)).ToArray();
                int n = Math.Max(0, (int)((logH - 20 * s) / lh));
                var shown = lines.Skip(Math.Max(0, lines.Length - n)).ToArray();
                if (shown.Length == 0)
                    Theme.Label(g, "THE SETUP LOG APPEARS HERE.", FM(11.5f * s), A(Theme.Dim), x + 14 * s, logY + 10 * s, lh, 1.2f * s);
                for (int i = 0; i < shown.Length; i++)
                    Theme.DrawText(g, shown[i], FM(11.5f * s), A(Theme.Dim), x + 14 * s, logY + 10 * s + i * lh, cw - 28 * s, lh);
                PopFade(g, st);
            }

            {
                var st = PushReveal(g, 4);
                float bx = x, by = H - 104 * s, bh = 46 * s;
                var f = FB(13 * s, true);
                string a = "DOWNLOAD MISSING", b = "DOWNLOAD AGAIN";
                float wa = Theme.TextWidth(g, a, f, 1.3f * s) + 52 * s;
                float wb = Theme.TextWidth(g, b, f, 1.3f * s) + 52 * s;
                bool busy = _setupModding;
                Theme.ButtonAnimated(g, a, bx, by, wa, bh, s, true, Hov("setup_start", _hover == "setup_start"), !busy, 13 * s, _alpha);
                if (!busy) AddHit("setup_start", bx, by, wa, bh);
                Theme.ButtonAnimated(g, b, bx + wa + 12 * s, by, wb, bh, s, false, Hov("setup_again", _hover == "setup_again"), !busy, 13 * s, _alpha);
                if (!busy) AddHit("setup_again", bx + wa + 12 * s, by, wb, bh);
                PopFade(g, st);
            }
        }

        // .bar: a slanted track with a fill
        void DrawBar(Graphics g, float x, float y, float w, float h, float frac, Color fill)
        {
            using (var p = Theme.Para(x, y, w, h, h * 0.5f))
            using (var b = new SolidBrush(A(White(26))))
                g.FillPath(b, p);
            frac = Clamp01(frac);
            if (frac <= 0) return;
            using (var p = Theme.Para(x, y, Math.Max(h, w * frac), h, h * 0.5f))
            using (var b = new SolidBrush(A(fill)))
                g.FillPath(b, p);
        }

        // ---------------------------------------------------------------------------
        // Settings
        void ScrollSettings(float to)
        {
            float next = Math.Clamp(to, 0, Math.Max(0, _settingsMaxScroll));
            if (Math.Abs(next - _settingsScroll) < 0.5f) return;
            CommitEdit();
            _openDropdownKey = null;
            _dropdownHoverIndex = -1;
            _settingsScroll = next;
            Invalidate();
        }

        void EnsureSettingsVisible()
        {
            if (_rowTop == null || _sel < 0 || _sel >= _rowTop.Length || _settingsViewH <= 0) return;
            float top = _rowTop[_sel];
            float bottom = top + _settingsRowH;
            if (top < _settingsScroll) ScrollSettings(top);
            else if (bottom > _settingsScroll + _settingsViewH) ScrollSettings(bottom - _settingsViewH);
        }

        void PaintSettings(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "03 / SETTINGS", "SETTINGS");

            var vis = GetVisibleSettings();
            float headerH = 34 * s, rowH = 74 * s;
            _settingsRowH = rowH;
            int tab = _settingsTabs.Length == 0 ? 0 : Math.Clamp(_settingsTab, 0, _settingsTabs.Length - 1);
            _settingsTab = tab;
            int pageStart = SectionStart(tab);
            int pageCount = tab < _settingsTabs.Length ? _settingsTabs[tab].SettingKeys.Length : 0;
            if (pageCount > 0 && (_sel < pageStart || _sel >= pageStart + pageCount)) _sel = pageStart;
            var rowTops = new float[vis.Length];
            float content = headerH;
            int cursor = pageStart;
            if (tab < _settingsTabs.Length)
            {
                foreach (var key in _settingsTabs[tab].SettingKeys)
                {
                    if (cursor >= vis.Length || vis[cursor].Key != key) continue;
                    rowTops[cursor] = content;
                    content += rowH;
                    cursor++;
                }
            }
            content += 12 * s;
            _sectionTop = new[] { 0f };
            _rowTop = rowTops;

            float listTop = ContentTop * s + 52 * s;
            float listBottom = H - StatusH * s - 8 * s;
            float viewH = Math.Max(rowH, listBottom - listTop);
            _settingsViewTop = listTop;
            _settingsViewH = viewH;
            _settingsMaxScroll = Math.Max(0, content - viewH);
            if (_settingsScroll > _settingsMaxScroll) _settingsScroll = _settingsMaxScroll;

            int active = tab;

            {
                float ty = ContentTop * s - 4 * s, th = 38 * s;
                float px = 12 * s, gap = 6 * s, pad = 28 * s;
                Font f = FM(px);
                float total = _settingsTabs.Sum(tb => Theme.TextWidth(g, tb.Name.ToUpperInvariant(), f, px * 0.1f) + pad) + gap * (_settingsTabs.Length - 1);
                if (total > cw) { px *= cw / total; f = FM(px); pad *= cw / Math.Max(total, 1); }
                float tx = x;
                for (int t = 0; t < _settingsTabs.Length; t++)
                {
                    string id = $"tab:{t}", name = _settingsTabs[t].Name.ToUpperInvariant();
                    bool on = t == active;
                    var st = PushReveal(g, t);
                    float tw = Theme.TextWidth(g, name, f, px * 0.1f) + pad;
                    float hv = Hov(id, _hover == id);
                    using (var p = Theme.Para(tx, ty, tw, th, 6 * s))
                    {
                        if (on) using (var b = new SolidBrush(A(Theme.Text))) g.FillPath(b, p);
                        using (var pen = new Pen(A(on ? Theme.Text : Theme.Mix(Theme.Line, Theme.Text, hv)), Math.Max(1, s))) g.DrawPath(pen, p);
                    }
                    Theme.Label(g, name, f, A(on ? Theme.Bg : Theme.Mix(Theme.Muted, Theme.Text, hv)), tx, ty, th, px * 0.1f, StringAlignment.Center, tw);
                    AddHit(id, tx, ty, tw, th);
                    PopFade(g, st);
                    tx += tw + gap;
                }
            }

            _volTrackValid = false;
            Font valueFont = FM(13 * s);
            float valueSpacing = 13 * s * 0.06f;
            float longestValue = 0;
            foreach (var set in _settings)
            {
                if (set.Labels == null) continue;
                for (int vi = 0; vi < set.Labels.Length; vi++)
                {
                    string lab = set.Labels[vi];
                    if (string.IsNullOrEmpty(lab)) continue;
                    longestValue = Math.Max(longestValue, Theme.TextWidth(g, set.Text ? lab : lab.ToUpperInvariant(), valueFont, valueSpacing));
                }
            }
            float ctlW = Math.Clamp(longestValue + 72 * s, 260 * s, Math.Max(260 * s, cw - 340 * s));
            float ctlX = x + cw - ctlW, ctlH = 40 * s;
            var clip = g.Save();
            g.SetClip(new RectangleF(x - 8 * s, listTop, cw + 36 * s, viewH));
            HLine(g, x, x + cw, listTop + (0 - _settingsScroll));
            int openIdx = -1;
            cursor = pageStart;
            if (tab < _settingsTabs.Length)
            {
                float hy = listTop + (0 - _settingsScroll);
                string name = _settingsTabs[tab].Name.ToUpperInvariant();
                Font hf = FM(12 * s);
                Theme.Label(g, name, hf, A(Theme.Accent), x, hy, headerH, 12 * s * 0.16f);
                float rule = x + Theme.TextWidth(g, name, hf, 12 * s * 0.16f) + 14 * s;
                if (rule < x + cw) HLine(g, rule, x + cw, hy + headerH / 2);
                int local = 0;
                foreach (var key in _settingsTabs[tab].SettingKeys)
                {
                    if (cursor >= vis.Length || vis[cursor].Key != key) continue;
                    var set = vis[cursor];
                    int idx = Array.IndexOf(_settings, set);
                    int i = cursor;
                    cursor++;
                    local++;
                    string id = $"set:{idx}";
                    bool isOpen = _openDropdownKey == set.Key;
                    if (isOpen) openIdx = i;
                    bool enabled = SettingOn(set);
                    bool on = enabled && (_hover == id || _hover == id + ":dd" || isOpen || (_keys && _sel == i));
                    float e = Theme.Ease(Hov(id, on));
                    float ry = listTop + (rowTops[i] - _settingsScroll);
                    bool inView = ry + rowH > listTop && ry < listTop + viewH;
                    var st = PushReveal(g, i + 1);

                    RowWash(g, x, ry, cw, rowH, e);
                    float shift = 16 * s * e;
                    var num = TextPath(local.ToString("00"), 40 * s);
                    var nb = num.GetBounds();
                    var ns = g.Save();
                    g.TranslateTransform(x + shift, ry + (rowH - nb.Height) / 2 - nb.Top);
                    using (var pen = new Pen(A(Theme.Mix(Theme.Dim, Theme.Accent, e)), Math.Max(1, 1.5f * s))) g.DrawPath(pen, num);
                    g.Restore(ns);

                    float lx = x + 76 * s + shift;
                    Theme.Label(g, set.Label.ToUpperInvariant(), FD(24 * s), A(enabled ? Theme.Text : Theme.Dim), lx, ry + 14 * s, 22 * s, 0.24f * s);
                    Theme.DrawText(g, SettingInfo.GetValueOrDefault(set.Key, ""), FB(13.5f * s), A(enabled ? Theme.Muted : Theme.Dim), lx, ry + 42 * s, ctlX - lx - 24 * s, 20 * s);

                    float cy = ry + (rowH - ctlH) / 2;
                    string val = ShownValue(set);
                    if (set.Text && inView)
                        _editRects[set.Key] = new Rectangle((int)ctlX, (int)cy, (int)ctlW, (int)ctlH);
                    if (set.Slider)
                    {
                        if (inView) PaintVolume(g, ctlX, cy, ctlW, ctlH);
                    }
                    else
                    {
                        float fh = Hov(id + ":dd", enabled && (_hover == id + ":dd" || isOpen));
                        using (var p = Theme.Para(ctlX, cy, ctlW, ctlH, 8 * s))
                        {
                            using (var b = new SolidBrush(A(White(14 + (int)(10 * fh))))) g.FillPath(b, p);
                            bool editingBox = set.Text && _editingKey == set.Key;
                        using (var pen = new Pen(A(isOpen || editingBox ? Theme.Accent : Theme.Mix(Theme.Line, White(150), fh)), Math.Max(1, s))) g.DrawPath(pen, p);
                        }
                        bool editing = set.Text && _editingKey == set.Key;
                        bool hint = set.Text && string.IsNullOrEmpty(_iniValues.GetValueOrDefault(set.Key, ""));
                        if (!editing)
                        {
                            var valueClip = g.Save();
                            g.SetClip(new RectangleF(ctlX + 14 * s, cy, Math.Max(1, ctlW - (set.Text ? 28 * s : 52 * s)), ctlH), CombineMode.Intersect);
                            Theme.Label(g, set.Text ? val : val.ToUpperInvariant(), valueFont, A(!enabled ? Theme.Dim : hint ? Theme.Muted : Theme.Text), ctlX + 20 * s, cy, ctlH, valueSpacing);
                            g.Restore(valueClip);
                        }
                        if (!set.Text)
                        {
                            float chx = ctlX + ctlW - 32 * s, chy = cy + ctlH / 2, ca = 4.5f * s, dir = isOpen ? -1 : 1;
                            using (var pen = new Pen(A(isOpen ? Theme.Accent : Theme.Mix(Theme.Muted, Theme.Text, fh)), Math.Max(1.2f, 1.6f * s)))
                                g.DrawLines(pen, new[] { new PointF(chx - ca, chy - ca * 0.5f * dir), new PointF(chx, chy + ca * 0.5f * dir), new PointF(chx + ca, chy - ca * 0.5f * dir) });
                        }
                    }
                    if (enabled && inView)
                    {
                        AddHit(id, x, Math.Max(ry, listTop), cw, Math.Min(ry + rowH, listTop + viewH) - Math.Max(ry, listTop));
                        if (set.Slider) AddHit("vol:" + set.Key, ctlX, cy, ctlW, ctlH);
                        else AddHit(id + ":dd", ctlX, cy, ctlW, ctlH);
                    }
                    HLine(g, x, x + cw, ry + rowH);
                    PopFade(g, st);
                }
            }
            g.Restore(clip);

            if (_settingsMaxScroll > 0)
            {
                float trackX = x + cw + 16 * s;
                float thumbH = Math.Max(28 * s, viewH * viewH / content);
                float thumbY = listTop + (viewH - thumbH) * (_settingsScroll / _settingsMaxScroll);
                _settingsTrackTop = listTop;
                _settingsTrackH = viewH;
                _settingsThumbH = thumbH;
                using (var b = new SolidBrush(White(28))) g.FillRectangle(b, trackX, listTop, 3 * s, viewH);
                using (var b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, trackX, thumbY, 3 * s, thumbH);
                AddHit("setscroll", trackX - 10 * s, listTop, 24 * s, viewH);
            }

            if (_editingKey != null && _settingEdit != null && _settingEdit.Visible && _editRects.TryGetValue(_editingKey, out var editAt))
                FitSettingEdit(editAt);

            if (openIdx >= 0)
            {
                float ry = listTop + (rowTops[openIdx] - _settingsScroll);
                PaintDropdown(g, H, vis[openIdx], Array.IndexOf(_settings, vis[openIdx]), ctlX, ry + (rowH - ctlH) / 2, ctlW, ctlH);
            }
        }
        void PaintVolume(Graphics g, float ctlX, float cy, float ctlW, float ctlH)
        {
            float s = _s;
            int vol = 100;
            int.TryParse(_iniValues.GetValueOrDefault("volume", "100"), out vol);
            vol = Math.Clamp(vol, 0, 100);
            float pad = 16 * s, labelW = 52 * s;
            float trackX = ctlX + pad;
            float trackW = Math.Max(20 * s, ctlW - pad - labelW - 8 * s);
            float trackH = Math.Max(3 * s, 4 * s);
            float mid = cy + ctlH / 2;
            _volTrackX = trackX;
            _volTrackW = trackW;
            _volTrackY = cy;
            _volTrackH = ctlH;
            _volTrackValid = true;
            using (var b = new SolidBrush(A(White(40)))) g.FillRectangle(b, trackX, mid - trackH / 2, trackW, trackH);
            using (var b = new SolidBrush(A(Theme.Accent))) g.FillRectangle(b, trackX, mid - trackH / 2, trackW * vol / 100f, trackH);
            float r = 7 * s, tx = trackX + trackW * vol / 100f;
            using (var b = new SolidBrush(A(Theme.Text))) g.FillEllipse(b, tx - r, mid - r, r * 2, r * 2);
            Theme.Label(g, vol.ToString(), FM(13 * s), A(Theme.Text), ctlX + ctlW - labelW - 10 * s, cy, ctlH, 13 * s * 0.04f, StringAlignment.Far, labelW);
        }

        void PaintDropdown(Graphics g, int H, Setting set, int idx, float fx, float fy, float fw, float fh)
        {
            float s = _s;
            var vals = set.Values;
            const int MaxRows = 8;
            int count = Math.Min(vals.Length, MaxRows);
            string current = _iniValues.GetValueOrDefault(set.Key, vals.Length > 0 ? vals[0] : "");
            if (_ddFirst < 0) _ddFirst = Math.Max(0, Array.IndexOf(vals, current) - MaxRows / 2);
            if (_dropdownHoverIndex >= 0 && _dropdownHoverIndex < _ddFirst) _ddFirst = _dropdownHoverIndex;
            if (_dropdownHoverIndex >= _ddFirst + MaxRows) _ddFirst = _dropdownHoverIndex - MaxRows + 1;
            _ddFirst = Math.Clamp(_ddFirst, 0, Math.Max(0, vals.Length - MaxRows));

            float itemH = 34 * s, listH = count * itemH + 8 * s;
            float ly = fy + fh + 6 * s;
            bool up = ly + listH > H - StatusH * s - 8 * s;
            if (up) ly = fy - 6 * s - listH;
            float e = Theme.Ease(Clamp01((float)(Now - _ddAt) / 0.25f));

            var st = g.Save();
            float shown = listH * e;
            g.SetClip(new RectangleF(fx - 2 * s, up ? ly + listH - shown : ly, fw + 4 * s, shown + 2 * s), CombineMode.Intersect);
            using (var p = Theme.CutCorner(fx, ly, fw, listH, 14 * s))
            {
                using (var b = new SolidBrush(Theme.Bg2)) g.FillPath(b, p);
                using (var pen = new Pen(White(60), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            for (int r = 0; r < count; r++)
            {
                int vi = _ddFirst + r;
                float iy = ly + 4 * s + r * itemH;
                bool hot = _dropdownHoverIndex == vi, sel = vals[vi] == current;
                if (hot)
                {
                    using (var b = new SolidBrush(Color.FromArgb(46, Theme.Accent))) g.FillRectangle(b, fx + 1, iy, fw - 2, itemH);
                    using (var b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, fx + 1, iy, 3 * s, itemH);
                }
                string opt = set.Labels != null && vi < set.Labels.Length ? set.Labels[vi] : vals[vi];
                var optClip = g.Save();
                g.SetClip(new RectangleF(fx + 14 * s, iy, Math.Max(1, fw - 48 * s), itemH), CombineMode.Intersect);
                Theme.Label(g, opt.ToUpperInvariant(), FM(13 * s), hot || sel ? Theme.Text : Theme.Muted, fx + 20 * s, iy, itemH, 13 * s * 0.06f);
                g.Restore(optClip);
                if (sel)
                    using (var b = new SolidBrush(Theme.Accent))
                        g.FillPolygon(b, new[] { new PointF(fx + fw - 28 * s, iy + itemH / 2 - 4 * s), new PointF(fx + fw - 18 * s, iy + itemH / 2 - 4 * s), new PointF(fx + fw - 21 * s, iy + itemH / 2 + 4 * s), new PointF(fx + fw - 31 * s, iy + itemH / 2 + 4 * s) });
                AddHit($"set:{idx}:opt:{vi}", fx, iy, fw, itemH);
            }
            if (vals.Length > MaxRows)
            {
                float trackH = count * itemH, thumbH = trackH * MaxRows / vals.Length;
                float thumbY = ly + 4 * s + (trackH - thumbH) * _ddFirst / (vals.Length - MaxRows);
                using var b = new SolidBrush(Theme.Accent);
                g.FillRectangle(b, fx + fw - 5 * s, thumbY, 2 * s, thumbH);
            }
            g.Restore(st);
        }
    }
}
