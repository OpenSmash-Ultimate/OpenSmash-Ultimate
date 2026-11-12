using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    // ============================================================================
    // FirstRunForm: shown when OpenSmash.exe (the release's OpenSmash.exe)
    // starts with no project around it and none set up before. It asks where
    // OpenSmash goes, downloads the project from GitHub into that folder
    // (ProjectSetup.InstallAsync), makes the shortcuts, and hands the folder to
    // Program, which opens the launcher there (the installer first: no build yet).
    // Same chrome as InstallerForm (Theme); a folder with other files is never
    // used without asking.
    // ============================================================================
    public sealed class FirstRunForm : Form
    {
        // The project folder once it is ready; "" when the window was closed first.
        public string Folder { get; private set; } = "";

        string _target;
        Image? _logo;
        Bitmap? _bg;
        float _s = 1;

        // choose | work | fail
        string _view = "choose";
        bool _optDesktop = true, _optStartMenu = true;
        string _phase = "";
        long _done, _total = -1;
        string _errMsg = "";
        CancellationTokenSource? _cts;
        string _status = "OpenSmash is not set up on this PC yet.";
        Color _statusColor = Theme.Muted;

        readonly List<(string Id, RectangleF R)> _hits = new();
        string _hover = "", _pressed = "";
        sealed class Prompt { public string Title = "", Body = "", Yes = "OK", No = ""; public Action? OnYes, OnNo; }
        Prompt? _modal;
        readonly System.Windows.Forms.Timer _timer = new() { Interval = 100 };

        public FirstRunForm(string workDir, string folder)
        {
            _target = folder;
            Text = "OpenSmash-Ultimate Setup";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Bg;
            DoubleBuffered = true;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            ShowInTaskbar = true;
            KeyPreview = true;
            ClientSize = Theme.WindowSize();
            DpiChanged += (s, e) => { Bounds = e.SuggestedRectangle; Invalidate(); };
            try
            {
                string ico = Path.Combine(workDir, "opensmash.ico");
                if (File.Exists(ico)) Icon = new Icon(ico);
                string logo = Path.Combine(workDir, "emblem.png");
                if (File.Exists(logo)) _logo = Image.FromFile(logo);
            }
            catch { }

            Paint += OnPaint;
            MouseMove += OnMouseMove;
            MouseLeave += (s, e) => { if (_hover != "") { _hover = ""; Cursor = Cursors.Default; Invalidate(); } };
            MouseDown += OnMouseDown;
            MouseUp += OnMouseUp;
            KeyDown += OnKeyDown;
            FormClosing += OnFormClosing;
            _timer.Tick += (s, e) => { if (_view == "work") Invalidate(); };
            Shown += async (s, e) =>
            {
                Opacity = 0;
                for (int i = 1; i <= 12; i++) { Opacity = i / 12.0; await Task.Delay(12); }
                Opacity = 1;
                _timer.Start();
            };
        }

        // ---------------------------------------------------------------------------
        // What the buttons do
        string FolderNote(out Color color)
        {
            switch (ProjectSetup.Inspect(_target))
            {
                case ProjectSetup.FolderKind.Missing: color = Theme.Ready; return "A new folder: OpenSmash goes here.";
                case ProjectSetup.FolderKind.Empty: color = Theme.Ready; return "Empty: OpenSmash goes here.";
                case ProjectSetup.FolderKind.Project: color = Theme.Ready; return "OpenSmash is already here: it is used as it is, nothing is downloaded.";
                default: color = Theme.Warn; return "This folder has other files. Download asks before anything goes near them.";
            }
        }

        void Browse()
        {
            using var dlg = new FolderBrowserDialog { Description = "Where OpenSmash goes (about 60 GB free for the build)", UseDescriptionForTitle = true, ShowNewFolderButton = true };
            string? start = Directory.Exists(_target) ? _target : Path.GetDirectoryName(_target);
            if (start != null && Directory.Exists(start)) dlg.SelectedPath = start;
            if (dlg.ShowDialog(this) == DialogResult.OK) _target = dlg.SelectedPath;
        }

        void StartDownload()
        {
            if (ProjectSetup.Inspect(_target) == ProjectSetup.FolderKind.Other)
            {
                // the user's files stay where they are: OpenSmash goes into a new folder inside, or elsewhere
                string inside = ProjectSetup.DefaultFolder(_target);
                _modal = new Prompt
                {
                    Title = "THIS FOLDER IS NOT EMPTY",
                    Body = _target + " has other files in it, and OpenSmash does not put its own among them.\n\nUse a new folder inside it instead: " + inside + " ?",
                    Yes = "USE THE NEW FOLDER", No = "CANCEL",
                    OnYes = () => { _target = inside; StartDownload(); },
                };
                return;
            }
            _ = RunDownload();
        }

        async Task RunDownload()
        {
            _view = "work";
            _phase = "download"; _done = 0; _total = -1; _errMsg = "";
            SetStatus("Downloading OpenSmash from GitHub…", Theme.Muted);
            _cts = new CancellationTokenSource();
            string folder = _target;
            try
            {
                await ProjectSetup.InstallAsync(folder, ProjectSetup.MainZipUrl, (phase, done, total) =>
                {
                    _phase = phase; _done = done; _total = total;
                }, _cts.Token);
            }
            catch (OperationCanceledException)
            {
                _view = "choose";
                SetStatus("Stopped. Nothing was left behind in the folder.", Theme.Muted);
                return;
            }
            catch (Exception ex)
            {
                _view = "fail";
                _errMsg = ex is HttpRequestException ? "GitHub could not be reached (" + ex.Message + "). Check the internet connection and try again." : ex.Message;
                SetStatus("The download stopped.", Theme.Bad);
                return;
            }
            finally { _cts?.Dispose(); _cts = null; Invalidate(); }
            Finish(folder);
        }

        void Finish(string folder)
        {
            try
            {
                ProjectSetup.Remember(folder);
                ProjectSetup.CopyLauncher(folder);
                ProjectSetup.MakeShortcuts(folder, _optDesktop ? ProjectSetup.DesktopDir() : "", _optStartMenu ? ProjectSetup.StartMenuDir() : "");
            }
            catch { }   // (a shortcut that could not be made does not stop the setup; the installer makes one too)
            Folder = folder;
            Close();
        }

        void UseExisting()
        {
            using var dlg = new FolderBrowserDialog { Description = "The OpenSmash folder you already have (with tools\\install_windowsmash.ps1)", UseDescriptionForTitle = true };
            if (dlg.ShowDialog(this) != DialogResult.OK) return;
            if (!ProjectSetup.IsProject(dlg.SelectedPath))
            {
                _modal = new Prompt { Title = "NOT AN OPENSMASH FOLDER", Body = dlg.SelectedPath + " has no tools\\install_windowsmash.ps1. Choose the folder you downloaded or cloned OpenSmash into." };
                return;
            }
            Finish(dlg.SelectedPath);
        }

        void SetStatus(string text, Color color) { _status = text; _statusColor = color; Invalidate(); }

        void OnFormClosing(object? sender, FormClosingEventArgs e)
        {
            if (_view == "work" && _cts != null)
            {
                e.Cancel = true;
                _modal = new Prompt
                {
                    Title = "STOP THE DOWNLOAD?", Body = "What was downloaded so far is removed again. The next start begins from the folder choice.",
                    Yes = "STOP", No = "GO ON", OnYes = () => _cts?.Cancel(),
                };
                Invalidate();
                return;
            }
            _timer.Stop();
        }

        // ---------------------------------------------------------------------------
        // Painting (InstallerForm's layout: header, cards, actions, footer)
        void Hit(string id, float x, float y, float w, float h) => _hits.Add((id, new RectangleF(x, y, w, h)));

        string HitAt(Point p)
        {
            for (int i = _hits.Count - 1; i >= 0; i--)
                if (_hits[i].R.Contains(p)) return _hits[i].Id;
            return "";
        }

        void OnPaint(object? sender, PaintEventArgs e)
        {
            var g = e.Graphics;
            int W = ClientSize.Width, H = ClientSize.Height;
            _s = H / 720f;
            _hits.Clear();
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            if (_bg == null || _bg.Width != W || _bg.Height != H)
            {
                _bg?.Dispose();
                _bg = Theme.MakeBackground(W, H, _logo);
            }
            g.DrawImageUnscaled(_bg, 0, 0);

            if (_view == "choose") PaintChoose(g, W);
            else PaintWork(g, W);

            PaintFooter(g, W, H);
            Theme.CloseButton(g, W, _s, _hover == "close", out var close);
            _hits.Add(("close", close));
            if (_modal != null) PaintModal(g, W, H);
        }

        void PaintHeader(Graphics g, int W, string title, string subtitle)
        {
            float s = _s, left = 96 * s;
            float ey = 34 * s, eh = 16 * s, k = 2 * s * 0.577f;
            using (var b = new SolidBrush(Theme.Accent))
                g.FillPolygon(b, new[] { new PointF(left + k, ey + eh / 2 - s), new PointF(left + 34 * s + k, ey + eh / 2 - s), new PointF(left + 34 * s, ey + eh / 2 + s), new PointF(left, ey + eh / 2 + s) });
            using (var f = Theme.Mono(13 * s))
                Theme.Label(g, "OPENSMASH SETUP", f, Theme.Accent, left + 46 * s, ey, eh, 13 * s * 0.2f);
            using (var f = Theme.Display(46 * s))
            {
                float tw = Theme.Label(g, title, f, Theme.Text, left, 60 * s, 38 * s, 0.4f * s);
                Theme.Label(g, ".", f, Theme.Accent, left + tw + 0.4f * s, 60 * s, 38 * s);
            }
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawText(g, subtitle, f, Theme.Muted, left, 106 * s, W - 2 * left - 60 * s, 22 * s);
        }

        void SectionLabel(Graphics g, string text, float x, float y)
        {
            using var f = Theme.Mono(11 * _s);
            Theme.Label(g, text, f, Theme.Dim, x, y, 18 * _s, 11 * _s * 0.16f);
        }

        void StatusMark(Graphics g, Color c, float x, float cy)
        {
            float s = _s;
            using var b = new SolidBrush(c);
            g.FillPolygon(b, new[] { new PointF(x + 2 * s, cy - 3 * s), new PointF(x + 8 * s, cy - 3 * s), new PointF(x + 6 * s, cy + 3 * s), new PointF(x, cy + 3 * s) });
        }

        void Pill(Graphics g, string id, string label, float x, float y, float w, float h)
        {
            Theme.Button(g, label, x, y, w, h, _s, primary: false, hot: _hover == id, true, 11 * _s);
            Hit(id, x, y, w, h);
        }

        float ActionButton(Graphics g, string id, string label, float x, float y, bool primary)
        {
            float s = _s, w, h;
            if (primary) { w = 230 * s; h = 46 * s; }
            else
            {
                using var f = Theme.Body(13 * s, true);
                w = Math.Max(124 * s, g.MeasureString(label, f).Width + 34 * s);
                h = 36 * s;
                y += 5 * s;
            }
            Theme.Button(g, label, x, y, w, h, s, primary, _hover == id);
            Hit(id, x, y, w, h);
            return x + w + 12 * s;
        }

        void PaintOption(Graphics g, string id, string label, bool on, float x, float y, float w, float h)
        {
            float s = _s, bs = 14 * s, bx = x + 16 * s, by = y + (h - bs) / 2;
            using (var p = Theme.Para(bx, by, bs + 3 * s, bs, 3 * s))
            {
                if (on)
                {
                    using (var b = new SolidBrush(Theme.Accent)) g.FillPath(b, p);
                    using var pen = new Pen(Color.White, Math.Max(1.4f, 1.8f * s)) { LineJoin = LineJoin.Miter };
                    float cx = bx + 1.5f * s;
                    g.DrawLines(pen, new[] { new PointF(cx + bs * 0.24f, by + bs * 0.52f), new PointF(cx + bs * 0.43f, by + bs * 0.72f), new PointF(cx + bs * 0.77f, by + bs * 0.30f) });
                }
                else
                    using (var pen = new Pen(_hover == id ? Theme.Text : Theme.Dim, Math.Max(1, s))) g.DrawPath(pen, p);
            }
            using (var f = Theme.Body(12.5f * s))
                Theme.DrawWrapped(g, label, f, on ? Theme.Text : Theme.Muted, x + 40 * s, y + 2 * s, w - 52 * s, h - 4 * s);
            Hit(id, x, y, w, h);
        }

        void PaintChoose(Graphics g, int W)
        {
            float s = _s, left = 96 * s, right = W - 96 * s;
            float rightW = 400 * s, gap = 24 * s, leftW = right - left - rightW - gap, rx = left + leftW + gap;
            PaintHeader(g, W, "GET OPENSMASH", "This setup downloads OpenSmash from GitHub into a folder, then sets up the game from your own files.");

            SectionLabel(g, "INSTALL TO", left, 138 * s);
            float y = 158 * s, h = 104 * s;
            Theme.Card(g, left, y, leftW, h, 10 * s, s);
            using (var f = Theme.Display(16 * s))
                Theme.Label(g, "OPENSMASH FOLDER", f, Theme.Text, left + 14 * s, y + 8 * s, 18 * s, 0.16f * s);
            using (var f = Theme.Body(11.5f * s))
                Theme.DrawText(g, "The project, then the unpacked game and the build: about 60 GB free. Not the Desktop or Documents.", f, Theme.Muted, left + 14 * s, y + 28 * s, leftW - 28 * s, 16 * s);
            float bw = 78 * s, bh = 26 * s, bx = left + leftW - 12 * s - bw;
            using (var f = Theme.Body(12.5f * s))
                Theme.DrawPath(g, _target, f, Theme.Text, left + 14 * s, y + 46 * s, bx - left - 22 * s, 22 * s);
            Pill(g, "pick", "BROWSE", bx, y + 44 * s, bw, bh);
            string note = FolderNote(out Color nc);
            StatusMark(g, nc, left + 16 * s, y + 84 * s);
            using (var f = Theme.Body(11.5f * s))
                Theme.DrawText(g, note, f, nc, left + 30 * s, y + 74 * s, leftW - 44 * s, 20 * s);

            SectionLabel(g, "WHAT HAPPENS", left, y + h + 18 * s);
            using (var f = Theme.Body(12.5f * s))
                Theme.DrawWrapped(g,
                    UiLocale.Text("1.  OpenSmash's files come from GitHub (the newest version of the main branch, about 20 MB).\n") +
                    UiLocale.Text("2.  OpenSmash.exe is copied into the folder and the shortcuts are made.\n") +
                    UiLocale.Text("3.  The launcher opens there and asks for your game files, keys and firmware, then builds the game (30 minutes to a few hours).\n\n") +
                    UiLocale.Text("Nothing outside the folder is changed, apart from the shortcuts."),
                    f, Theme.Muted, left + 4 * s, y + h + 42 * s, leftW - 8 * s, 150 * s);

            SectionLabel(g, "OPTIONS", rx, 138 * s);
            Theme.Card(g, rx, 158 * s, rightW, 120 * s, 10 * s, s);
            PaintOption(g, "opt:desktop", "Desktop shortcuts: OpenSmash and Play SSBU", _optDesktop, rx, 168 * s, rightW, 44 * s);
            PaintOption(g, "opt:start", "Start menu entries (an OpenSmash folder)", _optStartMenu, rx, 212 * s, rightW, 44 * s);

            float ay = 574 * s;
            float ax = ActionButton(g, "act:download", ProjectSetup.Inspect(_target) == ProjectSetup.FolderKind.Project ? "USE THIS FOLDER" : "DOWNLOAD AND SET UP", left, ay, true);
            ActionButton(g, "act:existing", "I ALREADY HAVE IT", ax, ay, false);
        }

        void PaintWork(Graphics g, int W)
        {
            float s = _s, left = 96 * s, right = W - 96 * s, rw = right - left;
            bool failed = _view == "fail";
            PaintHeader(g, W, failed ? "DOWNLOAD STOPPED" : _phase == "extract" ? "UNPACKING" : "DOWNLOADING", _target);

            double frac = _total > 0 ? Math.Clamp(_done / (double)_total, 0, 1) : 0;
            // the download is shown as the first 80 %, unpacking as the rest
            double shown = _phase == "extract" ? 0.8 + 0.2 * frac : 0.8 * frac;
            string big = _phase == "download" && _total <= 0 ? $"{_done / 1048576.0:N1} MB" : $"{(int)Math.Round(shown * 100)}%";
            using (var f = Theme.Display(60 * s))
                Theme.DrawText(g, big, f, Theme.Text, left - 4 * s, 132 * s, 420 * s, 70 * s);
            float barY = 206 * s, barH = 10 * s;
            using (var p = Theme.Para(left, barY, rw, barH, barH * 0.5f))
            using (var b = new SolidBrush(Color.FromArgb(26, 255, 255, 255)))
                g.FillPath(b, p);
            float fw = _phase == "download" && _total <= 0
                ? (float)(rw * 0.8 * (1 - Math.Exp(-_done / 20e6)))   // (GitHub does not say the size: a bar that slows down)
                : (float)(rw * shown);
            if (fw >= barH)
                using (var p = Theme.Para(left, barY, fw, barH, barH * 0.5f))
                using (var b = new SolidBrush(failed ? Color.FromArgb(255, 150, 60, 60) : Theme.Accent))
                    g.FillPath(b, p);
            string line = _phase == "extract" ? $"Unpacking  —  {_done:N0} / {_total:N0} files"
                : _total > 0 ? UiLocale.Interpolate($"Downloading from GitHub  —  {_done / 1048576.0:N1} / {_total / 1048576.0:N1} MB")
                : UiLocale.Interpolate($"Downloading from GitHub  —  {_done / 1048576.0:N1} MB");
            using (var f = Theme.Body(13.5f * s, true))
                Theme.DrawText(g, line, f, failed ? Theme.Bad : Theme.Text, left, 222 * s, rw, 26 * s);

            if (failed)
            {
                float ey = 268 * s, eh = 104 * s;
                Theme.Card(g, left, ey, rw, eh, 10 * s, s);
                using (var b = new SolidBrush(Theme.Bad)) g.FillRectangle(b, left + 1, ey + 1, 3 * s, eh - 2);
                using (var f = Theme.Display(17 * s))
                    Theme.DrawText(g, "WHAT STOPPED IT", f, Theme.Bad, left + 20 * s, ey + 10 * s, rw - 40 * s, 22 * s);
                using (var f = Theme.Body(13 * s, true))
                    Theme.DrawWrapped(g, _errMsg, f, Theme.Text, left + 20 * s, ey + 36 * s, rw - 40 * s, 60 * s);
            }

            float ay = 574 * s;
            if (failed)
            {
                float ax = ActionButton(g, "act:download", "RETRY", left, ay, true);
                ActionButton(g, "act:back", "BACK", ax, ay, false);
            }
            else ActionButton(g, "act:stop", "STOP", left, ay, false);
        }

        void PaintFooter(Graphics g, int W, int H)
        {
            float s = _s, left = 96 * s, bottom = H - 54 * s;
            StatusMark(g, _view == "work" ? Theme.Accent : _statusColor == Theme.Muted ? Theme.Ready : _statusColor, left, bottom + 14 * s);
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawText(g, _status, f, _statusColor, left + 15 * s, bottom, W - 2 * left - 340 * s, 28 * s);
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawText(g, "Setup", f, Theme.Muted, W - left - 260 * s, bottom - 14 * s, 260 * s, 22 * s, StringAlignment.Far);
            using (var f = Theme.Body(12 * s))
                Theme.DrawText(g, "OpenSmash-Ultimate — native PC port", f, Theme.Dim, W - left - 320 * s, bottom + 8 * s, 320 * s, 20 * s, StringAlignment.Far);
        }

        void PaintModal(Graphics g, int W, int H)
        {
            var m = _modal!;
            float s = _s;
            _hits.Clear();
            using (var b = new SolidBrush(Color.FromArgb(180, 0, 0, 0))) g.FillRectangle(b, 0, 0, W, H);
            float mw = 600 * s, pad = 26 * s, bodyH;
            using (var f = Theme.Body(13.5f * s))
                bodyH = Math.Min(260 * s, g.MeasureString(m.Body, f, (int)(mw - 2 * pad)).Height + 4 * s);
            float mh = pad + 34 * s + 12 * s + bodyH + 24 * s + 44 * s + pad;
            float mx = (W - mw) / 2, my = (H - mh) / 2;
            using (var p = Theme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(Theme.Bg2)) g.FillPath(b, p);
                using (var pen = new Pen(Theme.Line, Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(Theme.Accent)) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            using (var f = Theme.Display(28 * s))
                Theme.Label(g, m.Title, f, Theme.Text, mx + pad, my + pad + 4 * s, 26 * s, 0.28f * s);
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawWrapped(g, m.Body, f, Theme.Muted, mx + pad, my + pad + 46 * s, mw - 2 * pad, bodyH);
            float gap = 12 * s, bh = 44 * s, by = my + mh - pad - bh;
            float bw = Math.Min(220 * s, (mw - 2 * pad - gap) / 2);
            float yesX = mx + mw - pad - bw;
            Theme.Button(g, m.Yes, yesX, by, bw, bh, s, primary: true, hot: _hover == "modal:yes");
            Hit("modal:yes", yesX, by, bw, bh);
            if (m.No != "")
            {
                Theme.Button(g, m.No, yesX - gap - bw, by, bw, bh, s, primary: false, hot: _hover == "modal:no");
                Hit("modal:no", yesX - gap - bw, by, bw, bh);
            }
        }

        // ---------------------------------------------------------------------------
        // Input
        void OnMouseMove(object? sender, MouseEventArgs e)
        {
            string id = HitAt(e.Location);
            if (id == _hover) return;
            _hover = id;
            Cursor = id != "" ? Cursors.Hand : Cursors.Default;
            Invalidate();
        }

        void OnMouseDown(object? sender, MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            string id = HitAt(e.Location);
            if (id != "") { _pressed = id; return; }
            if (_modal != null) return;
            ReleaseCapture();
            SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);     // drag the borderless window
        }

        void OnMouseUp(object? sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && _pressed != "" && HitAt(e.Location) == _pressed) Activate(_pressed);
            _pressed = "";
        }

        void OnKeyDown(object? sender, KeyEventArgs e)
        {
            if (_modal != null)
            {
                if (e.KeyCode == Keys.Escape) Activate(_modal.No != "" ? "modal:no" : "modal:yes");
                else if (e.KeyCode == Keys.Enter) Activate("modal:yes");
                return;
            }
            if (e.KeyCode == Keys.Escape) Close();
        }

        void Activate(string id)
        {
            if (_modal != null && id != "modal:yes" && id != "modal:no") return;
            switch (id)
            {
                case "close": Close(); break;
                case "modal:yes": { var m = _modal; _modal = null; m?.OnYes?.Invoke(); break; }
                case "modal:no": { var m = _modal; _modal = null; m?.OnNo?.Invoke(); break; }
                case "pick": Browse(); break;
                case "opt:desktop": _optDesktop = !_optDesktop; break;
                case "opt:start": _optStartMenu = !_optStartMenu; break;
                case "act:download":
                    if (ProjectSetup.Inspect(_target) == ProjectSetup.FolderKind.Project) Finish(_target);
                    else StartDownload();
                    break;
                case "act:existing": UseExisting(); break;
                case "act:stop": _cts?.Cancel(); break;
                case "act:back": _view = "choose"; SetStatus("OpenSmash is not set up on this PC yet.", Theme.Muted); break;
            }
            _hover = "";
            if (!IsDisposed) Invalidate();
        }

        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _timer.Dispose();
                _bg?.Dispose();
                _logo?.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}
