// Original Windows installer layouts, now maintained in the shared launcher.
using System.Drawing;
using System.Globalization;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        void PaintHeader(Graphics g, int W, string eyebrow, string title, string subtitle)
        {
            float s = _s, left = 96 * s;

            // Eyebrow: a red skewed dash and mono red caps, as on the launcher's pages
            float ey = 34 * s, eh = 16 * s, k = 2 * s * 0.577f;
            using (var b = new SolidBrush(ArtworkTheme.Accent))
                g.FillPolygon(b, new[] { new PointF(left + k, ey + eh / 2 - s), new PointF(left + 34 * s + k, ey + eh / 2 - s), new PointF(left + 34 * s, ey + eh / 2 + s), new PointF(left, ey + eh / 2 + s) });
            using (var f = ArtworkTheme.Mono(13 * s))
                ArtworkTheme.Label(g, eyebrow, f, ArtworkTheme.Accent, left + 46 * s, ey, eh, 13 * s * 0.2f);

            // Title with the red period
            using (var f = ArtworkTheme.Display(46 * s))
            {
                float tw = ArtworkTheme.Label(g, title, f, ArtworkTheme.Text, left, 60 * s, 38 * s, 0.4f * s);
                ArtworkTheme.Label(g, ".", f, ArtworkTheme.Accent, left + tw + 0.4f * s, 60 * s, 38 * s);
            }
            using (var f = ArtworkTheme.Body(13.5f * s))
                ArtworkTheme.DrawText(g, subtitle, f, ArtworkTheme.Muted, left, 106 * s, W - 2 * left - 60 * s, 22 * s);
        }

        void SectionLabel(Graphics g, string text, float x, float y)
        {
            using var f = ArtworkTheme.Mono(11 * _s);
            ArtworkTheme.Label(g, text, f, ArtworkTheme.Dim, x, y, 18 * _s, 11 * _s * 0.16f);
        }

        void RowWash(Graphics g, float x, float y, float w, float h, int alpha = 36)
        {
            using (var lg = new LinearGradientBrush(new RectangleF(x - 1, y, w + 2, h), Color.FromArgb(alpha, ArtworkTheme.Accent), Color.FromArgb(0, ArtworkTheme.Accent), LinearGradientMode.Horizontal))
                g.FillRectangle(lg, x, y, w, h);
            using (var b = new SolidBrush(ArtworkTheme.Accent))
                g.FillRectangle(b, x, y, 3 * _s, h);
        }

        void StatusMark(Graphics g, Color c, float x, float cy)
        {
            float s = _s;
            using var b = new SolidBrush(c);
            g.FillPolygon(b, new[] { new PointF(x + 2 * s, cy - 3 * s), new PointF(x + 8 * s, cy - 3 * s), new PointF(x + 6 * s, cy + 3 * s), new PointF(x, cy + 3 * s) });
        }

        void Pill(Graphics g, string id, string label, float x, float y, float w, float h, bool enabled = true)
        {
            ArtworkTheme.Button(g, label, x, y, w, h, _s, primary: false, hot: enabled && _hover == id, enabled, 11 * _s);
            if (enabled) Hit(id, x, y, w, h);
        }

        float ActionButton(Graphics g, string id, string label, float x, float y, bool primary, bool enabled = true)
        {
            float s = _s;
            float w, h;
            if (primary) { w = 230 * s; h = 46 * s; }
            else
            {
                using var f = ArtworkTheme.Body(13 * s, true);
                w = Math.Max(124 * s, g.MeasureString(label, f).Width + 34 * s);
                h = 36 * s;
                y += 5 * s;
            }
            ArtworkTheme.Button(g, label, x, y, w, h, s, primary, enabled && _hover == id, enabled);
            if (enabled) Hit(id, x, y, w, h);
            return x + w + 12 * s;
        }

        void PaintSetup(Graphics g, int W, int H)
        {
            float s = _s, left = 96 * s, right = W - 96 * s;
            float rightW = 400 * s, gap = 24 * s;
            float leftW = right - left - rightW - gap;
            float rx = left + leftW + gap;

            PaintHeader(g, W, "FIRST-TIME SETUP", "BUILD THE GAME",
                "Folders found on this PC are filled in. Change any of them with Browse.");

            SectionLabel(g, "YOUR FILES", left, 138 * s);
            float y = 158 * s;
            y = PaintPathRow(g, "game", "GAME FILES", "yuzu's or Ryujinx's game folder, or yours: the game (.xci / .nsp / .nca), its 13.0.5 update, DLC", _gamePath, _gameStatus, _gameOk, left, y, leftW);
            y = PaintPathRow(g, "keys", "KEYS", "The folder with prod.keys and title.keys", _keysPath, _keysStatus, _keysOk, left, y, leftW);
            y = PaintPathRow(g, "firmware", "FIRMWARE", "Optional: its .nca files give the console's own fonts (with Chinese, Japanese and Korean text)", _fwPath, _fwStatus, _fwOk, left, y, leftW);

            float nameY = y + 8 * s;
            using (var f = ArtworkTheme.Body(16 * s, true))
                ArtworkTheme.DrawText(g, "Username", f, ArtworkTheme.Text, left + 4 * s, nameY, 108 * s, 36 * s);
            // The native username control is arranged at these same coordinates by ClassicSurface.

            SectionLabel(g, "OPTIONS", rx, 138 * s);
            float oy = 158 * s;
            float cardH = 392 * s;
            ArtworkTheme.Card(g, rx, oy, rightW, cardH, 10 * s, s);
            float oh = 52 * s;
            oy += 10 * s;
            PaintOption(g, "opt:shortcuts", "Make a desktop shortcut", _optShortcuts, rx, oy, rightW, 36 * s, "A desktop shortcut that starts OpenSmash."); oy += 36 * s;
            PaintOption(g, "opt:firewall", (Project?.Platform == OpenSmash.Core.DesktopPlatform.Windows ? "Allow the game through Windows Firewall now (asks for administrator rights once; needed for playing online)" : "Allow the game through the system firewall (asks for authorization when needed for hosting rooms)"), _optFirewall, rx, oy, rightW, oh, "Needed for playing online. Asks for administrator rights once."); oy += oh;
            PaintOption(g, "opt:shaders", "After the build, prepare the shaders in this window (you pick low, medium, or high; stops the first-time stutter)", _optShaders, rx, oy, rightW, oh, "Stops the first-time stutter of effects. Progress shows in this window."); oy += oh + 4 * s;
            using (var f = ArtworkTheme.Body(12 * s))
                ArtworkTheme.DrawWrapped(g, "Then the game is built, which takes 30 minutes to a few hours. If it stops, run it again: it carries on where it stopped.", f, ArtworkTheme.Dim, rx + 16 * s, oy, rightW - 32 * s, 48 * s);
            oy += 54 * s;
            using (var pen = new Pen(ArtworkTheme.Line, Math.Max(1, s)))
                g.DrawLine(pen, rx + 16 * s, oy, rx + rightW - 16 * s, oy);
            oy += 10 * s;
            const string jobsTip = "Compilers running at once. Fewer leaves the PC usable.";
            using (var f = ArtworkTheme.Body(12.5f * s))
                ArtworkTheme.DrawText(g, "Compiler jobs", f, ArtworkTheme.Text, rx + 16 * s, oy, 120 * s, 28 * s);
            float jx = rx + 140 * s;
            Pill(g, "jobs:-", "\u2212", jx, oy + 1 * s, 26 * s, 26 * s, _jobs > 1);
            using (var f = ArtworkTheme.Body(13 * s, true))
                ArtworkTheme.DrawText(g, _jobs.ToString(CultureInfo.InvariantCulture), f, ArtworkTheme.Text, jx + 26 * s, oy, 36 * s, 28 * s, StringAlignment.Center);
            Pill(g, "jobs:+", "+", jx + 62 * s, oy + 1 * s, 26 * s, 26 * s, _jobs < 64);
            using (var f = ArtworkTheme.Body(11.5f * s))
                ArtworkTheme.DrawText(g, "fewer = PC stays usable", f, ArtworkTheme.Dim, jx + 98 * s, oy, rightW - (jx - rx) - 110 * s, 28 * s);
            _tips["jobs:-"] = _tips["jobs:+"] = jobsTip;
            PaintOption(g, "opt:updates", "Check for updates first", _optUpdates, rx, oy + 32 * s, rightW, 32 * s, "Looks for a newer WindowSmash on GitHub before building.");
            if (Project?.Platform == OpenSmash.Core.DesktopPlatform.Windows)
                PaintOption(g, "opt:license", "Accept Microsoft's license terms for the C runtime and Windows SDK (the compiler download, about 570 MB)", _optLicense, rx, oy + 64 * s, rightW, 72 * s, "Read them first: https://go.microsoft.com/fwlink/?LinkId=2327714");
            else using (var f = ArtworkTheme.Body(12.5f * s)) ArtworkTheme.DrawWrapped(g, "The platform installer checks and installs the build dependencies. System authorization is handled by your desktop; completed work is kept if setup stops.", f, ArtworkTheme.Muted, rx + 16 * s, oy + 70 * s, rightW - 32 * s, 68 * s);

            float ay = 574 * s;
            string label = _failedBefore || Project?.IsBuilt == true ? "RESUME"
                : _succeeded && _jobKind == "install" && !_jobDry ? "BUILD AGAIN"
                : "SET UP AND BUILD";
            float ax = ActionButton(g, "act:check", "CHECK FILES", left, ay, primary: false);
            ax = ActionButton(g, "act:install", label, ax, ay, primary: true);
            ActionButton(g, "act:logs", "OPEN LOGS", ax, ay, false);
        }

        float PaintPathRow(Graphics g, string id, string title, string hint, string path, string status, bool? ok, float x, float y, float w)
        {
            float s = _s, h = 112 * s, gap = 8 * s;
            ArtworkTheme.Card(g, x, y, w, h - gap, 10 * s, s);
            bool hot = _hover == "pick:" + id || _hover == "clear:" + id;
            if (hot) RowWash(g, x, y, w, h - gap, 22);
            using (var f = ArtworkTheme.Display(16 * s))
                ArtworkTheme.Label(g, title, f, ArtworkTheme.Text, x + 14 * s, y + 8 * s, 18 * s, 0.16f * s);
            using (var f = ArtworkTheme.Body(11.5f * s))
                ArtworkTheme.DrawText(g, hint, f, ArtworkTheme.Muted, x + 14 * s, y + 28 * s, w - 28 * s, 16 * s);
            string shown = string.IsNullOrEmpty(path) ? "Not chosen" : path;
            float bw = 78 * s, bh = 26 * s;
            int buttons = id == "firmware" && !string.IsNullOrEmpty(path) ? 2 : 1;
            float bx = x + w - 12 * s - buttons * bw - (buttons - 1) * 8 * s;
            using (var f = ArtworkTheme.Body(12.5f * s))
            {
                if (string.IsNullOrEmpty(path))
                    ArtworkTheme.DrawText(g, shown, f, ArtworkTheme.Dim, x + 14 * s, y + 46 * s, bx - x - 22 * s, 22 * s);
                else
                    ArtworkTheme.DrawPath(g, shown, f, ArtworkTheme.Text, x + 14 * s, y + 46 * s, bx - x - 22 * s, 22 * s);
            }
            if (buttons == 2) Pill(g, "clear:" + id, "CLEAR", bx, y + 44 * s, bw, bh);
            Pill(g, "pick:" + id, "BROWSE", x + w - 12 * s - bw, y + 44 * s, bw, bh);
            Color sc = ok == true ? ArtworkTheme.Ready : ok == false ? ArtworkTheme.Bad : ArtworkTheme.Dim;
            StatusMark(g, sc, x + 16 * s, y + 84 * s);
            using (var f = ArtworkTheme.Body(11.5f * s))
                ArtworkTheme.DrawWrapped(g, status, f, sc, x + 30 * s, y + 72 * s, w - 44 * s, 30 * s);
            return y + h;
        }

        void PaintOption(Graphics g, string id, string label, bool on, float x, float y, float w, float h, string tip)
        {
            float s = _s;
            bool hot = _hover == id;
            if (hot) RowWash(g, x + 1, y, w - 2, h, 28);
            float bs = 14 * s, bxx = x + 16 * s, byy = y + (h - bs) / 2;
            using (var p = ArtworkTheme.Para(bxx, byy, bs + 3 * s, bs, 3 * s))
            {
                if (on)
                {
                    using (var b = new SolidBrush(ArtworkTheme.Accent)) g.FillPath(b, p);
                    using var pen = new Pen(Color.White, Math.Max(1.4f, 1.8f * s)) { LineJoin = LineJoin.Miter };
                    float cx = bxx + 1.5f * s;
                    g.DrawLines(pen, new[] { new PointF(cx + bs * 0.24f, byy + bs * 0.52f), new PointF(cx + bs * 0.43f, byy + bs * 0.72f), new PointF(cx + bs * 0.77f, byy + bs * 0.30f) });
                }
                else
                {
                    using var pen = new Pen(hot ? ArtworkTheme.Text : ArtworkTheme.Dim, Math.Max(1, s));
                    g.DrawPath(pen, p);
                }
            }
            using (var f = ArtworkTheme.Body(12.5f * s))
                ArtworkTheme.DrawWrapped(g, label, f, on ? ArtworkTheme.Text : ArtworkTheme.Muted, x + 40 * s, y + 2 * s, w - 52 * s, h - 4 * s);
            Hit(id, x, y, w, h);
            _tips[id] = tip;
        }

        void PaintBuild(Graphics g, int W, int H)
        {
            float s = _s, left = 96 * s, right = W - 96 * s;
            bool failed = !_running && !_succeeded;

            string what = _jobKind == "bootstrap" ? "SETUP" : _jobKind == "update" ? "UPDATE" : _jobDry ? "DRY RUN" : "BUILD";
            string title = _running
                ? (_jobKind == "bootstrap" ? "GETTING READY" : _jobKind == "update" ? "UPDATING" : _jobDry ? "DRY RUN" : "BUILDING THE GAME")
                : _succeeded
                    ? (_jobKind == "install" && !_jobDry ? "READY TO PLAY" : what + " FINISHED")
                    : what + " STOPPED";
            string sub = _running
                ? (_jobKind == "bootstrap" ? "Setting up a private Python for the installer (one time, about 100 MB). Nothing is installed system-wide."
                   : _jobKind == "update" ? "Getting the latest version of OpenSmash."
                   : "This takes from 30 minutes to a few hours. You can close the window: the next start goes on from where it stopped.")
                : _succeeded ? _status
                : "Nothing that finished is lost. Fix the problem below, then resume.";
            PaintHeader(g, W, "FIRST-TIME SETUP", title, sub);

            // Steps
            float lw = 380 * s;
            SectionLabel(g, "STEPS", left, 142 * s);
            float y0 = 166 * s, y1 = 552 * s, rowH = 26 * s;
            int visible = Math.Max(1, (int)((y1 - y0) / rowH));
            if (_steps.Count > visible) visible--;                 // the last row says how many are hidden
            int focus = _steps.FindIndex(x => x.State == "run");
            if (focus < 0) focus = _steps.FindIndex(x => x.State == "fail");
            if (focus < 0) focus = _succeeded ? 0 : _steps.FindLastIndex(x => x.State != "wait");
            int first = Math.Clamp(focus - visible / 2, 0, Math.Max(0, _steps.Count - visible));
            int hidden = _steps.Count - Math.Min(_steps.Count, first + visible);
            if (hidden > 0)
                using (var f = ArtworkTheme.Body(12 * s))
                    ArtworkTheme.DrawText(g, $"\u2026and {hidden} more", f, ArtworkTheme.Dim, left + 30 * s, y0 + visible * rowH, lw - 40 * s, rowH);
            if (_steps.Count == 0)
                using (var f = ArtworkTheme.Body(13 * s))
                    ArtworkTheme.DrawText(g, _running ? "Waiting for the first step\u2026" : "No steps ran.", f, ArtworkTheme.Dim, left + 4 * s, y0, lw, rowH);
            for (int i = first; i < Math.Min(_steps.Count, first + visible); i++)
            {
                var st = _steps[i];
                float ry = y0 + (i - first) * rowH;
                if (st.State == "run") ArtworkTheme.FillAccentBar(g, left, ry + 1 * s, lw, rowH - 2 * s, 6 * s, 90);
                float dx = left + 12 * s, dy = ry + rowH / 2 - 4 * s, dd = 8 * s;
                Color dc = st.State switch
                {
                    "run" => Color.White, "done" => ArtworkTheme.Ready, "fail" => ArtworkTheme.Bad, "dry" => ArtworkTheme.Warn, _ => ArtworkTheme.Dim
                };
                if (st.State == "wait")
                    using (var pen = new Pen(ArtworkTheme.Dim, Math.Max(1, 1.2f * s))) g.DrawEllipse(pen, dx, dy, dd, dd);
                else
                    using (var b = new SolidBrush(dc)) g.FillEllipse(b, dx, dy, dd, dd);
                Color tc = st.State switch
                {
                    "run" => ArtworkTheme.Text, "done" => ArtworkTheme.Muted, "fail" => ArtworkTheme.Bad, "dry" => ArtworkTheme.Warn, _ => ArtworkTheme.Dim
                };
                using (var f = ArtworkTheme.Body(13 * s, st.State == "run"))
                    ArtworkTheme.DrawText(g, st.Title, f, tc, left + 30 * s, ry, lw - 110 * s, rowH);
                string t = st.State switch
                {
                    "run" when st.T0 != null => FmtDur((DateTime.Now - st.T0.Value).TotalSeconds),
                    "skip" => "skipped",
                    "dry" => "would run",
                    "fail" => "failed",
                    _ => st.Secs > 0 ? FmtDur(st.Secs) : ""
                };
                using (var f = ArtworkTheme.Body(12 * s))
                    ArtworkTheme.DrawText(g, t, f, st.State == "run" ? ArtworkTheme.Text : ArtworkTheme.Dim, left + lw - 84 * s, ry, 74 * s, rowH, StringAlignment.Far);
                Hit("step:" + i, left, ry, lw, rowH);
                _tips["step:" + i] = st.Title + (st.Msg != "" ? " \u2014 " + st.Msg : "");
            }

            // Progress
            float rx = left + lw + 48 * s, rw = right - rx;
            var curNow = _steps.FirstOrDefault(x => x.State == "run");
            double frac;
            if (_succeeded && !_jobDry && _steps.Count > 0) frac = 1.0;
            else if (curNow != null && curNow.Total > 0) frac = Math.Clamp(curNow.Done / curNow.Total, 0, 1);
            else
            {
                int n = _steps.Count(x => x.State is "done" or "skip" or "dry");
                frac = _steps.Count > 0 ? n / (double)_steps.Count : 0;
            }
            using (var f = ArtworkTheme.Display(60 * s))
                ArtworkTheme.DrawText(g, $"{(int)Math.Round(frac * 100)}%", f, ArtworkTheme.Text, rx - 4 * s, 132 * s, 260 * s, 70 * s);

            float barY = 206 * s, barH = 10 * s;
            using (var p = ArtworkTheme.Para(rx, barY, rw, barH, barH * 0.5f))
            using (var b = new SolidBrush(Color.FromArgb(26, 255, 255, 255)))
                g.FillPath(b, p);
            float fw = (float)(rw * frac);
            if (fw >= barH)
                using (var p = ArtworkTheme.Para(rx, barY, fw, barH, barH * 0.5f))
                using (var b = new SolidBrush(failed ? Color.FromArgb(255, 150, 60, 60) : _succeeded ? ArtworkTheme.Ready : ArtworkTheme.Accent))
                    g.FillPath(b, p);

            var cur = _steps.FirstOrDefault(x => x.State == "run") ?? _steps.FirstOrDefault(x => x.State == "fail");
            string curText = cur == null ? ""
                : cur.Total > 0
                    ? cur.Title + "  \u2014  " + (cur.Msg != "" ? cur.Msg + ", " : "") + $"{cur.Done:N0} / {cur.Total:N0}"
                    : cur.Title + (cur.Msg != "" ? "  \u2014  " + cur.Msg : "");
            string eta = _running
                ? "elapsed " + FmtDur((DateTime.Now - _jobStart).TotalSeconds)
                : _jobEnd > _jobStart ? $"took {FmtDur((_jobEnd - _jobStart).TotalSeconds)}" : "";
            if (_running && curNow != null && curNow.Total > 0 && curNow.Done > 0 && curNow.T0 != null)
            {
                double f = Math.Clamp(curNow.Done / curNow.Total, 0, 0.999);
                double elap = (DateTime.Now - curNow.T0.Value).TotalSeconds;
                if (f >= 0.03 && elap >= 8) eta += ", " + FmtLeft(elap * (1 - f) / f) + " on this step";
            }
            using (var f = ArtworkTheme.Body(13.5f * s, true))
                ArtworkTheme.DrawText(g, curText, f, failed ? ArtworkTheme.Bad : ArtworkTheme.Text, rx, 222 * s, rw - 260 * s, 26 * s);
            using (var f = ArtworkTheme.Body(12.5f * s))
                ArtworkTheme.DrawText(g, eta, f, ArtworkTheme.Muted, rx + rw - 260 * s, 222 * s, 260 * s, 26 * s, StringAlignment.Far);

            // What stopped it
            float logLabelY = 262 * s;
            if (failed)
            {
                float ey = 258 * s, eh = 104 * s;
                ArtworkTheme.Card(g, rx, ey, rw, eh, 10 * s, s);
                using (var p = ArtworkTheme.CutCorner(rx, ey, rw, eh, 16 * s))
                using (var pen = new Pen(Color.FromArgb(170, 255, 100, 100), Math.Max(1, s)))
                    g.DrawPath(pen, p);
                using (var b = new SolidBrush(ArtworkTheme.Bad)) g.FillRectangle(b, rx + 1, ey + 1, 3 * s, eh - 2);
                using (var f = ArtworkTheme.Display(17 * s))
                    ArtworkTheme.DrawText(g, "WHAT STOPPED IT", f, ArtworkTheme.Bad, rx + 20 * s, ey + 10 * s, rw - 40 * s, 22 * s);
                using (var f = ArtworkTheme.Body(13 * s, true))
                    ArtworkTheme.DrawWrapped(g, _errMsg, f, ArtworkTheme.Text, rx + 20 * s, ey + 36 * s, rw - 40 * s, 34 * s);
                using (var f = ArtworkTheme.Body(12.5f * s))
                    ArtworkTheme.DrawWrapped(g, _errHint, f, ArtworkTheme.Muted, rx + 20 * s, ey + 70 * s, rw - 40 * s, 30 * s);
                logLabelY = 374 * s;
            }

            // Log
            SectionLabel(g, "LOG", rx, logLabelY);
            float ly = logLabelY + 24 * s, lh = y1 - ly;
            ArtworkTheme.Card(g, rx, ly, rw, lh, 10 * s, s);
            Hit("log", rx, ly, rw, lh);
            float lineH = 15 * s;
            int rows = Math.Max(1, (int)((lh - 16 * s) / lineH));
            _logScroll = Math.Clamp(_logScroll, 0, Math.Max(0, _log.Count - rows));
            int start = Math.Max(0, _log.Count - rows - _logScroll);
            using (var f = ArtworkTheme.Mono(11.5f * s))
                for (int i = 0; i < rows && start + i < _log.Count; i++)
                {
                    string line = _log[start + i];
                    Color c = line.StartsWith("ERROR") || line.Contains("FAILED") ? ArtworkTheme.Bad
                        : line.StartsWith("  hint:") ? ArtworkTheme.Warn : ArtworkTheme.Dim;
                    ArtworkTheme.DrawText(g, line, f, c, rx + 14 * s, ly + 8 * s + i * lineH, rw - 28 * s, lineH);
                }

            // Actions
            float ay = 574 * s, ax = left;
            if (_running)
            {
                ax = ActionButton(g, "act:stop", "STOP", ax, ay, false);
                ActionButton(g, "act:logs", "OPEN LOGS", ax, ay, false);
            }
            else if (_succeeded)
            {
                if (_jobKind == "install" && !_jobDry)
                    ax = ActionButton(g, "act:continue", Installed ? "CONTINUE" : "CLOSE", ax, ay, true);
                else
                    ax = ActionButton(g, "act:back", "BACK", ax, ay, true);
                ActionButton(g, "act:logs", "OPEN LOGS", ax, ay, false);
            }
            else
            {
                ax = ActionButton(g, "act:resume", _jobKind == "install" ? "RESUME" : "RETRY", ax, ay, true);
                if (_jobKind != "bootstrap") ax = ActionButton(g, "act:back", "BACK TO FILES", ax, ay, false);
                if (_errAction == "msvc") ax = ActionButton(g, "act:msvc", "INSTALL BUILD TOOLS", ax, ay, false);
                ActionButton(g, "act:logs", "OPEN LOGS", ax, ay, false);
            }
        }

        void PaintFooter(Graphics g, int W, int H)
        {
            float s = _s, left = 96 * s, bottom = H - 54 * s;
            string text = _tips.TryGetValue(_hover, out var tip) && _modal == null ? tip : _status;
            Color col = text == _status ? (_statusColor == ArtworkTheme.Muted ? ArtworkTheme.Muted : Blend(_statusColor)) : ArtworkTheme.Text;
            Color dot = text != _status ? ArtworkTheme.Muted : _running ? ArtworkTheme.Accent : _statusColor == ArtworkTheme.Muted ? ArtworkTheme.Ready : _statusColor;
            StatusMark(g, dot, left, bottom + 14 * s);
            using (var f = ArtworkTheme.Body(13.5f * s))
                ArtworkTheme.DrawText(g, text, f, col, left + 15 * s, bottom, W - 2 * left - 340 * s, 28 * s);
            using (var f = ArtworkTheme.Body(13.5f * s))
                ArtworkTheme.DrawText(g, "First-time setup", f, ArtworkTheme.Muted, W - left - 260 * s, bottom - 14 * s, 260 * s, 22 * s, StringAlignment.Far);
            using (var f = ArtworkTheme.Body(12 * s))
                ArtworkTheme.DrawText(g, "OpenSmash-Ultimate \u2014 native PC port", f, ArtworkTheme.Dim, W - left - 320 * s, bottom + 8 * s, 320 * s, 20 * s, StringAlignment.Far);
        }

        void PaintModal(Graphics g, int W, int H)
        {
            var m = _modal!;
            float s = _s;
            _hits.Clear();
            using (var b = new SolidBrush(Color.FromArgb(180, 0, 0, 0))) g.FillRectangle(b, 0, 0, W, H);

            float mw = (m.Mid != "" ? 720 : 540) * s, pad = 26 * s;
            float bodyH;
            using (var f = ArtworkTheme.Body(13.5f * s))
                bodyH = Math.Min(260 * s, g.MeasureString(m.Body, f, (int)(mw - 2 * pad)).Height + 4 * s);
            float mh = pad + 34 * s + 12 * s + bodyH + 24 * s + 44 * s + pad;
            float mx = (W - mw) / 2, my = (H - mh) / 2;

            using (var p = ArtworkTheme.CutCorners(mx, my, mw, mh, 30 * s))
            {
                using (var b = new SolidBrush(ArtworkTheme.Bg2)) g.FillPath(b, p);
                using (var pen = new Pen(ArtworkTheme.Line, Math.Max(1, s))) g.DrawPath(pen, p);
                var cs = g.Save();
                g.SetClip(p, CombineMode.Intersect);
                using (var b = new SolidBrush(ArtworkTheme.Accent)) g.FillRectangle(b, mx, my, mw, 4 * s);
                g.Restore(cs);
            }
            using (var f = ArtworkTheme.Display(28 * s))
                ArtworkTheme.Label(g, m.Title.ToUpperInvariant(), f, ArtworkTheme.Text, mx + pad, my + pad + 4 * s, 26 * s, 0.28f * s);
            using (var f = ArtworkTheme.Body(13.5f * s))
                ArtworkTheme.DrawWrapped(g, m.Body, f, ArtworkTheme.Muted, mx + pad, my + pad + 46 * s, mw - 2 * pad, bodyH);

            float gap = 12 * s, bh = 44 * s, by = my + mh - pad - bh;
            int nBtn = 1 + (m.Mid != "" ? 1 : 0) + (m.No != "" ? 1 : 0);
            float bw = Math.Min(190 * s, (mw - 2 * pad - gap * (nBtn - 1)) / nBtn);
            float yesX = mx + mw - pad - bw;
            ArtworkTheme.Button(g, m.Yes, yesX, by, bw, bh, s, primary: true, hot: _hover == "modal:yes");
            Hit("modal:yes", yesX, by, bw, bh);
            float left = yesX;
            if (m.Mid != "")
            {
                left -= gap + bw;
                ArtworkTheme.Button(g, m.Mid, left, by, bw, bh, s, primary: false, hot: _hover == "modal:mid");
                Hit("modal:mid", left, by, bw, bh);
            }
            if (m.No != "")
            {
                left -= gap + bw;
                ArtworkTheme.Button(g, m.No, left, by, bw, bh, s, primary: false, hot: _hover == "modal:no");
                Hit("modal:no", left, by, bw, bh);
            }
        }
}
