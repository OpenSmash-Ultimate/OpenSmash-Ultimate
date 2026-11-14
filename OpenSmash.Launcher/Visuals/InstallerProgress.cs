// Installer progress parsing for the shared launcher.
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Globalization;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        void ResetSteps()
        {
            _steps.Clear();
            _overallProgress = 0;
            _jsonSeen = false; _fallbackN = 0;
            _errMsg = _errHint = _errAction = _doneMsg = "";
            _doneOk = null;
        }

        void UpdateOverallProgress()
        {
            double total = 0, done = 0;
            foreach (var step in _steps)
            {
                total += step.Est;
                double fraction = step.State is "done" or "skip" or "dry" ? 1
                    : step.Total > 0 ? Math.Clamp(step.Done / step.Total, 0, 1) : 0;
                done += step.Est * fraction;
            }
            // Unix shader preparation runs separately after the backend finishes.
            if (_optShaders && Project?.Platform != OpenSmash.Core.DesktopPlatform.Windows
                && !_steps.Any(step => step.Id == "shaders")) total += 600;
            if (total > 0) _overallProgress = Math.Max(_overallProgress, Math.Min(.99, done / total));
        }

        Step AddStep(string id, string title, double est)
        {
            var s = new Step { Id = id, Title = title, Est = est > 0 ? est : 10 };
            _steps.Add(s);
            return s;
        }

        void SetStepState(Step s, string state, double? secs, string msg)
        {
            s.State = state;
            if (state == "run" && s.T0 == null) s.T0 = DateTime.Now;
            if (secs is double v && v > 0) s.Secs = v;
            else if ((state is "done" or "fail") && s.T0 != null && s.Secs <= 0) s.Secs = (DateTime.Now - s.T0.Value).TotalSeconds;
            if (msg != "") s.Msg = msg;
            if (state == "done" && s.Total > 0) s.Done = s.Total;
        }

        void HandleEvent(JsonElement e)
        {
            _jsonSeen = true;
            switch (Str(e, "ev"))
            {
                case "plan":
                    _steps.Clear();
                    if (e.TryGetProperty("steps", out var list) && list.ValueKind == JsonValueKind.Array)
                        foreach (var p in list.EnumerateArray())
                        {
                            string id = Str(p, "id");
                            if (id == "") id = "s" + _steps.Count;
                            string title = Str(p, "title");
                            AddStep(id, title != "" ? title : id, Num(p, "est_s") ?? 0);
                        }
                    break;
                case "step":
                {
                    string id = Str(e, "id");
                    var s = GetStep(id) ?? AddStep(id, id, 10);
                    string m = Str(e, "msg");
                    double? secs = Num(e, "secs");
                    switch (Str(e, "state"))
                    {
                        case "start": SetStepState(s, "run", null, m); break;
                        case "done": SetStepState(s, "done", secs, m); break;
                        case "skip": SetStepState(s, "skip", secs, m); break;
                        case "dry": SetStepState(s, "dry", null, m); break;
                        case "fail":
                            SetStepState(s, "fail", secs, m);
                            if (m != "") Log($"[{s.Id}] FAILED: {m}");
                            break;
                    }
                    break;
                }
                case "progress":
                {
                    var s = GetStep(Str(e, "id"));
                    if (s == null) break;
                    if (Num(e, "done") is double d) s.Done = d;
                    if (Num(e, "total") is double t) s.Total = t;
                    s.EstP = Bool(e, "est") == true;
                    if (s.State == "wait") SetStepState(s, "run", null, "");
                    if (Str(e, "msg") != "") s.Msg = Str(e, "msg");
                    break;
                }
                case "done":
                    _doneOk = Bool(e, "ok");
                    _doneMsg = Str(e, "msg");
                    if (_doneMsg != "") Log(_doneMsg);
                    break;
                case "error":
                    _errMsg = Str(e, "msg") != "" ? Str(e, "msg") : "The installation stopped.";
                    _errHint = Str(e, "hint");
                    _errAction = Str(e, "action");
                    Log("ERROR: " + _errMsg);
                    if (_errHint != "") Log("  hint: " + _errHint);
                    break;
            }
        }

        static string ShortTitle(string title)
        {
            int p = title.IndexOf(" (", StringComparison.Ordinal);
            return p > 8 ? title[..p] : title;
        }

        static double EstFor(string title)
        {
            string t = title.ToLowerInvariant();
            if (t.Contains("build the game")) return 5400;
            if (t.Contains("shader")) return 600;
            if (t.Contains("compiler") || t.Contains("python package")) return 180;
            if (t.Contains("fighter")) return 120;
            if (t.Contains("font")) return 90;
            if (t.Contains("connect your game")) return 45;
            if (t.Contains("copy the game")) return 40;
            return 30;
        }

        static bool TryCount(string s, out double n)
        {
            s = s.Replace(",", "").Replace(" ", "");
            return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out n);
        }

        bool ParseSetupLine(string line)
        {
            var m = Regex.Match(line, @"^\[(\d+)/(\d+)\]\s+(.+)$");
            if (!m.Success) return false;
            int i = int.Parse(m.Groups[1].Value), n = int.Parse(m.Groups[2].Value);
            string title = m.Groups[3].Value.Trim();
            if (n < 1 || n > 40 || i < 1 || i > n || title == "") return false;
            if (_steps.Count == 0 || _fallbackN != n)
            {
                _steps.Clear(); _fallbackN = n;
                for (int k = 1; k <= n; k++) AddStep("s" + k, "step " + k, 30);
            }
            for (int k = 0; k < i - 1; k++)
                if (_steps[k].State is "wait" or "run") SetStepState(_steps[k], "done", null, "");
            var s = _steps[i - 1];
            s.Title = ShortTitle(title);
            s.Est = EstFor(title);
            s.Done = 0; s.Total = 0;
            _genModules.Clear();
            _genTotal = 0;
            if (s.State != "run") SetStepState(s, "run", null, "");
            for (int k = i; k < _steps.Count; k++)
                if (_steps[k].State == "run") _steps[k].State = "wait";
            var tail = Regex.Match(title, @":\s*ok\b|would run|skipped", RegexOptions.IgnoreCase);
            if (tail.Success)
            {
                if (title.Contains("would run", StringComparison.OrdinalIgnoreCase)) SetStepState(s, "dry", null, "");
                else if (title.Contains("skipped", StringComparison.OrdinalIgnoreCase)) SetStepState(s, "skip", null, "done earlier");
                else SetStepState(s, "done", null, "");
            }
            return true;
        }

        void NoteGen(Step run, double atLeast)
        {
            if (_genTotal <= 0) return;
            run.Done = Math.Max(atLeast, _genModules.Count);
            run.Total = _genTotal;
            run.Msg = "writing the C code";
        }

        void ApplyMeasure(string line)
        {
            var run = _steps.LastOrDefault(s => s.State == "run");
            if (run == null) return;
            var writing = Regex.Match(line, @"writing the C code:\s*([^,]+),\s*([\d,]+)\s+of\s+([\d,]+)\s+modules", RegexOptions.IgnoreCase);
            if (writing.Success && TryCount(writing.Groups[2].Value, out double idx) && TryCount(writing.Groups[3].Value, out double modules) && modules > 0)
            {
                _genTotal = (int)modules;
                _genModules.Add(writing.Groups[1].Value.Trim());
                NoteGen(run, idx);
                return;
            }
            var summary = Regex.Match(line, @"^([A-Za-z0-9_]+):\s+\d+\s+files,\s+[\d,.]+\s+MB of C\b");
            if (summary.Success)
            {
                _genModules.Add(summary.Groups[1].Value);
                NoteGen(run, _genModules.Count);
                return;
            }
            var reading = Regex.Match(line, @"(listing|reading) shader files:\s*([\d,]+)\s+of\s+([\d,]+)", RegexOptions.IgnoreCase);
            if (reading.Success && TryCount(reading.Groups[2].Value, out double rd) && TryCount(reading.Groups[3].Value, out double rt) && rt > 0)
            {
                run.Done = rd; run.Total = rt; run.Msg = reading.Groups[1].Value.ToLowerInvariant() + " shader files";
                return;
            }
            var shaders = Regex.Match(line, @"([\d,]+)\s+of\s+([\d,]+)\s+shaders", RegexOptions.IgnoreCase);
            if (shaders.Success && TryCount(shaders.Groups[1].Value, out double sd) && TryCount(shaders.Groups[2].Value, out double stotal) && stotal > 0)
            {
                run.Done = sd; run.Total = stotal; run.Msg = "compiling shaders";
                return;
            }
            var blocks = Regex.Match(line, @"([\d,]+)\s+of\s+([\d,]+)\s+blocks", RegexOptions.IgnoreCase);
            if (blocks.Success && TryCount(blocks.Groups[1].Value, out double d) && TryCount(blocks.Groups[2].Value, out double t) && t > 0)
            {
                run.Done = d; run.Total = t; run.Msg = "verified";
                return;
            }
            var bar = Regex.Match(line, @"\[[#\-]+\]\s+(\d+)\s*%");
            if (bar.Success && int.TryParse(bar.Groups[1].Value, out int pct) && !line.Contains("writing the C code", StringComparison.OrdinalIgnoreCase))
            {
                run.Done = pct; run.Total = 100;
                if (line.Contains("linking", StringComparison.OrdinalIgnoreCase)) run.Msg = "linking";
                else if (line.Contains("compiling", StringComparison.OrdinalIgnoreCase)) run.Msg = "compiling";
            }
        }

        static string FmtDur(double secs)
        {
            var t = TimeSpan.FromSeconds(Math.Round(Math.Max(0, secs)));
            return t.TotalHours >= 1 ? $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}" : $"{(int)t.TotalMinutes}:{t.Seconds:00}";
        }

        static string FmtLeft(double secs)
        {
            if (secs < 60) return "less than a minute left";
            if (secs < 5400) return $"about {(int)Math.Round(secs / 60)} min left";
            double h = Math.Floor(secs / 3600);
            int m = (int)Math.Round((secs - h * 3600) / 60);
            if (m >= 60) { h += 1; m = 0; }
            return $"about {(int)h} h {m} min left";
        }
}
