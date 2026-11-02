using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    // ============================================================================
    // InstallerForm: the first-run window, shown instead of the launcher while no
    // build exists. Same chrome as LauncherForm (Theme): it looks for the user's
    // game files (find_inputs.py --json), checks the PC (precheck.py --json) and
    // runs install.py --progress-json, whose "##ssbu {json}" lines drive the step
    // list and the progress bar (event schema: scripts\installer_window.ps1).
    // Without Python it first runs scripts\bootstrap.ps1 -NoLaunch.
    // ============================================================================
    public sealed class InstallerForm : Form
    {
        // True once an install finished and the game exe exists: Program then opens the launcher.
        public bool Installed { get; private set; }

        readonly string _repo;
        readonly string _defaultBuild;
        readonly string _locFile;
        readonly string _logFile;
        string _storedBuild;
        string _py = "windowsmash";
        Image? _logo;
        Bitmap? _bg;
        float _s = 1;

        // ---------------------------------------------------------------------------
        // Inputs
        sealed class Field
        {
            public string Key = "", Label = "", Tag = "", Hint = "", Value = "";
            public string Status = "";
            public Color StatusColor = Theme.Dim;
        }
        readonly Dictionary<string, Field> _f = new();
        static readonly string[] FieldOrder = { "base", "update", "prodkeys", "firmware", "dlc", "buildloc" };
        readonly List<string> _dlc = new();
        JsonElement? _found;

        // Options
        bool _optShortcuts = true, _optUpdates = true, _optRuntime = true, _optFast, _optDry;
        bool _optFirewall = true, _optShaders = true, _optLicense = true;
        string _gamePath = "", _keysPath = "", _fwPath = "", _onlineName = "";
        string _gameStatus = "Looking…", _keysStatus = "Looking…", _fwStatus = "Looking…";
        bool? _gameOk, _keysOk, _fwOk;
        int _scanGen;
        TextBox? _nameBox;
        int _jobs, _jobsSafe;

        // PC check
        sealed class Check { public string Level = "", What = "", Msg = ""; }
        List<Check> _checks = new();
        string _checkNote = "Checking this PC\u2026";
        double _buildGb = 8.0, _dumpGb = 17.0;     // precheck.py sends its own numbers

        // Background processes
        Proc? _scanFind, _scanPre, _job;
        bool _foundDone, _preDone, _preAgain;
        DateTime? _paramsChangedAt;

        // Job (install / update / bootstrap)
        string _view = "setup";                     // setup | build
        string _jobKind = "";
        bool _jobDry, _running, _succeeded, _failedBefore;
        DateTime _jobStart, _jobEnd;
        sealed class Step
        {
            public string Id = "", Title = "", State = "wait", Msg = "";
            public double Est = 10, Secs, Done, Total;
            public bool EstP;
            public DateTime? T0;
        }
        readonly List<Step> _steps = new();
        bool _jsonSeen;
        int _fallbackN;
        readonly HashSet<string> _genModules = new(StringComparer.OrdinalIgnoreCase);
        int _genTotal;
        string _errMsg = "", _errHint = "", _errAction = "", _doneMsg = "";
        bool? _doneOk;

        // setup.py's own steps, by their share of a first build (used only when no plan event arrives)
        static readonly Dictionary<string, double> FallbackWeights = new()
        {
            ["deps"] = 1, ["stage"] = 1, ["convert"] = 1, ["modules"] = 2, ["tables"] = 1, ["lift"] = 15,
            ["sdk"] = 8, ["compile"] = 55, ["nro"] = 12, ["link"] = 3, ["check"] = 1
        };

        // Log + status line
        readonly List<string> _log = new();
        readonly StringBuilder _filePending = new();
        int _logScroll;
        string _status = "Looking for your game files\u2026";
        Color _statusColor = Theme.Muted;

        // Hit-testing, hover text, modal
        readonly List<(string Id, RectangleF R)> _hits = new();
        readonly Dictionary<string, string> _tips = new();
        string _hover = "", _pressed = "";
        sealed class Prompt { public string Title = "", Body = "", Yes = "OK", Mid = "", No = ""; public Action? OnYes, OnMid, OnNo; }
        bool _licenseAsked;
        Prompt? _modal;

        readonly System.Windows.Forms.Timer _timer = new() { Interval = 100 };
        int _tick;
        bool _dirty = true;
        bool _closeConfirmed;

        // ---------------------------------------------------------------------------
        public InstallerForm(string workDir, string repo)
        {
            _repo = repo;
            _defaultBuild = Path.Combine(repo, "build");
            _locFile = Path.Combine(repo, "build_location.txt");
            _storedBuild = GetStoredBuild();
            string buildDir = _storedBuild != "" ? _storedBuild : _defaultBuild;
            string logDir = Path.Combine(buildDir, "logs");
            try { if (!Directory.Exists(Path.GetPathRoot(logDir))) logDir = Path.Combine(_defaultBuild, "logs"); } catch { logDir = Path.Combine(_defaultBuild, "logs"); }
            _logFile = Path.Combine(logDir, "installer_window.log");
            _jobs = Math.Max(1, Math.Min(24, Environment.ProcessorCount - 2));

            AddField("base", "BASE GAME", "REQUIRED", "Your game: .xci / .nsp / .nsz / .nca, or a folder you already extracted (with exefs\\ and romfs\\).");
            AddField("update", "UPDATE", "REQUIRED", "The 13.0.4 or 13.0.5 update (.nsp / .nsz).");
            AddField("prodkeys", "PROD.KEYS", "REQUIRED", "Your console keys: needed to read package files.");
            AddField("firmware", "FIRMWARE", "OPTIONAL", "Only the fonts change. With your firmware: the console's own fonts, Chinese / Japanese / Korean text included. Without: open-source fonts (same game, Asian characters missing).");
            AddField("dlc", "DLC", "OPTIONAL", "Extra packages (costumes, fighters, spirits). Not needed to run: they only unlock their content.");
            AddField("buildloc", "INSTALL TO", "ABOUT 25 GB", "Where the unpacked game and the build go. Choose a folder on a drive with enough free space.");
            _f["buildloc"].Value = buildDir;

            Text = "OpenSmash-Ultimate";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Bg;
            DoubleBuffered = true;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            ShowInTaskbar = true;
            KeyPreview = true;
            ClientSize = Theme.WindowSize();
            _nameBox = new TextBox
            {
                BorderStyle = BorderStyle.FixedSingle,
                BackColor = Color.FromArgb(16, 16, 18),
                ForeColor = Color.FromArgb(236, 236, 236),
                MaxLength = 32,
                Visible = false,
                PlaceholderText = "type username here",
                Font = new Font("Segoe UI", 15f, FontStyle.Regular, GraphicsUnit.Pixel),
            };
            _nameBox.TextChanged += (_, _) => _onlineName = _nameBox.Text;
            Controls.Add(_nameBox);
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
            MouseWheel += OnMouseWheel;
            KeyDown += OnKeyDown;
            FormClosing += OnFormClosing;
            _timer.Tick += (s, e) => OnTick();

            Shown += async (s, e) =>
            {
                Opacity = 0;
                for (int i = 1; i <= 12; i++) { Opacity = i / 12.0; await Task.Delay(12); }
                Opacity = 1;
                _checkNote = "Folders found on this PC are filled in.";
                try { DetectFolders(); CheckLocal(); _ = CheckGameAsync(); }
                catch { SetStatus("Choose your game files, keys and firmware.", Theme.Muted); }
                _timer.Start();
            };

            UpdateFieldStatus();
        }

        void AddField(string key, string label, string tag, string hint) =>
            _f[key] = new Field { Key = key, Label = label, Tag = tag, Hint = hint };

        string Val(string key) => _f[key].Value.Trim().Trim('"');
        string Script(string name) => Path.Combine(_repo, "scripts", name);

        // ---------------------------------------------------------------------------
        // Paths
        string GetStoredBuild()
        {
            try
            {
                if (!File.Exists(_locFile)) return "";
                string? t = File.ReadLines(_locFile, Encoding.UTF8).FirstOrDefault()?.Trim().Trim('"');
                return string.IsNullOrEmpty(t) ? "" : Environment.ExpandEnvironmentVariables(t);
            }
            catch { return ""; }
        }

        string ResolvePython()
        {
            string p = Path.Combine(_repo, "deps", "python", "python.exe");
            if (File.Exists(p)) return p;
            try
            {
                string f = Path.Combine(_defaultBuild, "python_path.txt");
                if (File.Exists(f))
                {
                    string? t = File.ReadLines(f).FirstOrDefault()?.Trim().Trim('"');
                    if (!string.IsNullOrEmpty(t) && File.Exists(t)) return t;
                }
            }
            catch { }
            return "";
        }

        static string FullPath(string p)
        {
            try { return Path.GetFullPath(p).TrimEnd('\\'); } catch { return p.TrimEnd('\\'); }
        }
        static bool SamePath(string a, string b) => string.Equals(FullPath(a), FullPath(b), StringComparison.OrdinalIgnoreCase);
        static bool IsDir(string p) => p != "" && Directory.Exists(p);
        static bool IsFile(string p) => p != "" && File.Exists(p);

        string GetChosenBuild()
        {
            string v = Val("buildloc");
            return v == "" ? _defaultBuild : FullPath(Environment.ExpandEnvironmentVariables(v));
        }

        static double? GetFreeGB(string p)
        {
            try
            {
                string? root = Path.GetPathRoot(FullPath(p));
                if (string.IsNullOrEmpty(root) || root.StartsWith(@"\\")) return null;
                var d = new DriveInfo(root);
                if (!d.IsReady) return null;
                return d.AvailableFreeSpace / 1073741824.0;
            }
            catch { return null; }
        }

        // The same sum as precheck.py check_disk.
        (double Gb, string Text) GetNeedGB()
        {
            string loc = GetChosenBuild(), b = Val("base");
            double n = _buildGb;
            var parts = new List<string> { $"build {_buildGb:N0} GB" };
            bool haveDump = IsDir(b) || File.Exists(Path.Combine(loc, "dump", "romfs", "data.arc"));
            if (!haveDump)
            {
                n += _dumpGb; parts.Add($"unpacked game {_dumpGb:N0} GB");
                double t = 0;
                foreach (var p in new[] { b, Val("update") })
                    if (p.EndsWith(".nsz", StringComparison.OrdinalIgnoreCase) && IsFile(p)) t += new FileInfo(p).Length;
                if (t > 0)
                {
                    double tg = Math.Round(t * 1.25 / 1073741824.0, 1);
                    n += tg; parts.Add($"{tg:N0} GB for a while to decompress the .nsz");
                }
            }
            return (n, string.Join(" + ", parts));
        }

        bool HasState(string loc) =>
            File.Exists(Path.Combine(loc, "state.json")) || File.Exists(Path.Combine(loc, "install_state.json"));

        // ---------------------------------------------------------------------------
        // find_inputs.py results
        JsonElement? FindItem(string list, string path)
        {
            if (_found is not JsonElement r || path == "") return null;
            if (!r.TryGetProperty(list, out var arr) || arr.ValueKind != JsonValueKind.Array) return null;
            foreach (var it in arr.EnumerateArray())
                if (string.Equals(Str(it, "path"), path, StringComparison.OrdinalIgnoreCase)) return it;
            return null;
        }

        static string Str(JsonElement e, string name) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

        static bool? Bool(JsonElement e, string name) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False) ? v.GetBoolean() : null;

        static double? Num(JsonElement e, string name) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;

        // find_inputs' sentence without the long path in it.
        static string StripDesc(JsonElement it)
        {
            string d = Str(it, "desc"), p = Str(it, "path");
            if (p != "") d = d.Replace(p, Directory.Exists(p) ? "this folder" : "this file");
            d = Regex.Replace(d, @"\s*\(for tools[^)]*\)", "");
            d = Regex.Replace(d, @"^this (file|folder):\s*", "");
            d = d.Trim();
            return d.Length > 0 ? char.ToUpperInvariant(d[0]) + d[1..] : d;
        }

        void SetFieldStatus(string key, string text, Color color)
        {
            _f[key].Status = text;
            _f[key].StatusColor = color;
        }

        void UpdateFieldStatus()
        {
            bool searching = _scanFind != null && !_foundDone;
            string looking = "Looking for it on this PC\u2026";

            string v = Val("base");
            if (v == "") SetFieldStatus("base", searching ? looking : "Required: choose the game file, or a folder you already extracted.", searching ? Theme.Dim : Theme.Warn);
            else if (IsDir(v))
            {
                var it = FindItem("dumps", v);
                string fnt = !Directory.Exists(Path.Combine(v, "sysdata", "fonts")) && Val("firmware") == ""
                    ? " No fonts in this dump and no firmware: open-source fonts will be used." : "";
                if (it is JsonElement i) SetFieldStatus("base", "Dump folder: " + StripDesc(i) + fnt, Theme.Ready);
                else if (Directory.Exists(Path.Combine(v, "exefs")) && Directory.Exists(Path.Combine(v, "romfs"))) SetFieldStatus("base", "Dump folder: exefs and romfs found." + fnt, Theme.Ready);
                else SetFieldStatus("base", "This folder has no exefs\\ and romfs\\ inside: choose the game file instead, or the right folder.", Theme.Bad);
            }
            else if (IsFile(v))
            {
                if (FindItem("base", v) is JsonElement i) SetFieldStatus("base", StripDesc(i), Bool(i, "supported") == false ? Theme.Bad : Theme.Ready);
                else SetFieldStatus("base", $"{Path.GetExtension(v).ToLowerInvariant()}, {new FileInfo(v).Length / 1073741824.0:N2} GB (not checked yet; the installer reads it)", Theme.Info);
            }
            else SetFieldStatus("base", "This path does not exist.", Theme.Bad);

            bool baseIsDump = IsDir(v);
            v = Val("update");
            if (v == "")
            {
                if (baseIsDump) SetFieldStatus("update", "Not needed: the base game is an extracted dump.", Theme.Dim);
                else SetFieldStatus("update", searching ? looking : "Needed for the game version the port is built for (13.0.4 or 13.0.5).", searching ? Theme.Dim : Theme.Warn);
            }
            else if (IsFile(v))
            {
                if (FindItem("updates", v) is JsonElement i) SetFieldStatus("update", StripDesc(i), Bool(i, "supported") == false ? Theme.Bad : Theme.Ready);
                else SetFieldStatus("update", "File chosen (not checked yet; the installer reads it).", Theme.Info);
            }
            else SetFieldStatus("update", "This file does not exist.", Theme.Bad);

            v = Val("prodkeys");
            if (v == "")
            {
                if (baseIsDump) SetFieldStatus("prodkeys", "Not needed: the base game is an extracted dump.", Theme.Dim);
                else SetFieldStatus("prodkeys", searching ? looking : "Needed for package files (.nsp / .xci / .nsz).", searching ? Theme.Dim : Theme.Warn);
            }
            else if (IsFile(v))
            {
                if (FindItem("keys", v) is JsonElement i) SetFieldStatus("prodkeys", StripDesc(i), Bool(i, "complete") == false ? Theme.Warn : Theme.Ready);
                else SetFieldStatus("prodkeys", "File chosen (only the key names are looked at, never the values).", Theme.Info);
            }
            else SetFieldStatus("prodkeys", "This file does not exist.", Theme.Bad);

            v = Val("firmware");
            if (v == "") SetFieldStatus("firmware", searching ? looking : "Optional. Without it open-source fonts are used (no Chinese / Japanese / Korean characters).", Theme.Dim);
            else if (IsDir(v))
            {
                if (FindItem("firmware", v) is JsonElement i) SetFieldStatus("firmware", StripDesc(i), Theme.Ready);
                else SetFieldStatus("firmware", "Folder chosen (not checked yet; the installer reads it).", Theme.Info);
            }
            else SetFieldStatus("firmware", "This folder does not exist.", Theme.Bad);

            int n = _dlc.Count;
            if (n > 0 && baseIsDump) SetFieldStatus("dlc", "The base game is an extracted dump, so these packages are not used.", Theme.Warn);
            else if (n == 0) SetFieldStatus("dlc", searching ? looking : "None chosen. Optional: they only unlock their content.", Theme.Dim);
            else SetFieldStatus("dlc", $"{n} package{(n == 1 ? "" : "s")} will be read. Optional: the game runs without them.", Theme.Ready);

            UpdateLocStatus();
            _dirty = true;
        }

        void UpdateLocStatus()
        {
            string loc = GetChosenBuild();
            if (File.Exists(loc)) { SetFieldStatus("buildloc", "This is a file, not a folder.", Theme.Bad); return; }
            var need = GetNeedGB();
            double? free = GetFreeGB(loc);
            string root = Path.GetPathRoot(loc) ?? "";
            if (root != "" && !root.StartsWith(@"\\") && !Directory.Exists(root))
            {
                SetFieldStatus("buildloc", $"The drive {root} is not on this PC (or not connected): choose another folder.", Theme.Bad);
                return;
            }
            string t;
            Color c;
            if (free is not double fr) { t = $"Free space of {root} unknown; about {need.Gb:N0} GB needed ({need.Text})."; c = Theme.Warn; }
            else
            {
                t = $"{fr:N0} GB free on {root.TrimEnd('\\')}, about {need.Gb:N0} GB needed ({need.Text}).";
                if (fr < need.Gb) { c = Theme.Bad; t += " Not enough: choose a folder on another drive, or free some space."; }
                else if (fr < need.Gb * 1.25) { c = Theme.Warn; t += " It fits, but only just."; }
                else c = Theme.Ready;
            }
            if (_storedBuild != "" && !SamePath(loc, _storedBuild) && File.Exists(Path.Combine(_storedBuild, "state.json")))
            {
                t += $" Note: the last install is in {_storedBuild}; a new location starts from the beginning there.";
                if (c == Theme.Ready) c = Theme.Info;
            }
            else if (HasState(loc)) t += " An install is already here: Resume goes on with it.";
            SetFieldStatus("buildloc", t, c);
        }

        // ---------------------------------------------------------------------------
        // Pickers
        void SetValue(string key, string value)
        {
            _f[key].Value = value;
            UpdateFieldStatus();
            if (key is "base" or "update" or "buildloc") _paramsChangedAt = DateTime.Now;   // the PC check counts their sizes
        }

        void BrowseFile(string key, string title, string filter)
        {
            using var d = new OpenFileDialog { Title = title, Filter = filter, CheckFileExists = true };
            string cur = Val(key);
            if (IsFile(cur)) d.InitialDirectory = Path.GetDirectoryName(cur);
            if (d.ShowDialog(this) == DialogResult.OK) SetValue(key, d.FileName);
        }

        void BrowseFolder(string key, string title)
        {
            using var d = new FolderBrowserDialog { Description = title, UseDescriptionForTitle = true, ShowNewFolderButton = false };
            string cur = Val(key);
            if (IsDir(cur)) d.SelectedPath = cur;
            if (d.ShowDialog(this) == DialogResult.OK) SetValue(key, d.SelectedPath);
        }

        void BrowseLocation()
        {
            using var d = new FolderBrowserDialog
            {
                Description = "Install location: a folder on a drive with about 25 GB free (the unpacked game and the build go there)",
                UseDescriptionForTitle = true,
                ShowNewFolderButton = true
            };
            string cur = GetChosenBuild();
            if (IsDir(cur)) d.SelectedPath = cur;
            else if (IsDir(Path.GetDirectoryName(cur) ?? "")) d.SelectedPath = Path.GetDirectoryName(cur)!;
            if (d.ShowDialog(this) != DialogResult.OK) return;
            string p = d.SelectedPath;
            // A drive root gets its own folder: the build makes many folders and files there.
            if (string.Equals(Path.GetPathRoot(p), p, StringComparison.OrdinalIgnoreCase)) p = Path.Combine(p, "SSBU-native");
            SetValue("buildloc", p);
        }

        void AddDlc()
        {
            using var d = new OpenFileDialog { Title = "DLC packages (.nsp, .nsz)", Filter = "Switch DLC|*.nsp;*.nsz|All files|*.*", Multiselect = true };
            if (d.ShowDialog(this) != DialogResult.OK) return;
            foreach (var f in d.FileNames)
                if (!_dlc.Contains(f, StringComparer.OrdinalIgnoreCase)) _dlc.Add(f);
            UpdateFieldStatus();
        }

        // ---------------------------------------------------------------------------
        // Scans
        void StartScans()
        {
            if (_py == "") return;
            _foundDone = false;
            _scanFind = new Proc(capture: true);
            _scanFind.Start(_py, new[] { "-u", Script("find_inputs.py"), "--json" }, _repo);
            SetStatus("Looking for your game files\u2026", Theme.Muted);
            UpdateFieldStatus();
            StartPrecheck();
        }

        void StartPrecheck()
        {
            if (_py == "") return;
            if (_scanPre != null && !_scanPre.Done) { _preAgain = true; return; }
            _preDone = false;
            var a = new List<string> { "-u", Script("precheck.py"), "--json" };
            if (!_optRuntime) a.Add("--need-msvc");
            a.Add("--build"); a.Add(GetChosenBuild());      // the disk check looks at the drive of the install location
            string b = Val("base");
            if (IsDir(b)) { a.Add("--dump"); a.Add(b.TrimEnd('\\')); }
            else foreach (var g in new[] { b, Val("update") }) if (IsFile(g)) { a.Add("--game"); a.Add(g); }
            _scanPre = new Proc(capture: true);
            _scanPre.Start(_py, a, _repo);
            if (_checks.Count == 0) _checkNote = "Checking this PC\u2026";
        }

        void ApplyFound()
        {
            string raw = _scanFind!.Output;
            JsonElement? r = null;
            try
            {
                int i = raw.IndexOf('{');
                if (i >= 0) { using var doc = JsonDocument.Parse(raw[i..]); r = doc.RootElement.Clone(); }
            }
            catch { }
            if (r is not JsonElement root)
            {
                string err = _scanFind.ErrorText.Trim().Split('\n').LastOrDefault() ?? "";
                SetStatus($"Searching for your game files did not work (code {_scanFind.ExitCode}). You can still choose them yourself. {err}", Theme.Warn);
                Log("find_inputs failed: " + _scanFind.ErrorText);
                UpdateFieldStatus();
                return;
            }
            _found = root;
            string note = "";
            if (root.TryGetProperty("best", out var best) && best.ValueKind == JsonValueKind.Object)
            {
                if (Val("base") == "")
                {
                    if (Str(best, "dump") != "") { _f["base"].Value = Str(best, "dump"); note = Str(best, "base"); }
                    else if (Str(best, "base") != "") _f["base"].Value = Str(best, "base");
                }
                if (Val("update") == "" && Str(best, "update") != "") _f["update"].Value = Str(best, "update");
                if (Val("prodkeys") == "" && Str(best, "prod_keys") != "") _f["prodkeys"].Value = Str(best, "prod_keys");
                if (Val("firmware") == "" && Str(best, "firmware") != "") _f["firmware"].Value = Str(best, "firmware");
                if (_dlc.Count == 0 && best.TryGetProperty("dlc", out var dl) && dl.ValueKind == JsonValueKind.Array)
                    foreach (var d in dl.EnumerateArray())
                        if (d.ValueKind == JsonValueKind.String) _dlc.Add(d.GetString()!);
            }
            UpdateFieldStatus();
            if (note != "" && IsDir(Val("base")))
                _f["base"].Status += $" Your package {Path.GetFileName(note)} was found too: press FILE to use it instead.";

            int n = new[] { "base", "update", "prodkeys", "firmware" }.Count(k => Val(k) != "");
            double secs = Num(root, "seconds") ?? 0;
            SetStatus($"Search finished ({secs.ToString("0.0", CultureInfo.InvariantCulture)} s): {n} of 4 file fields filled in. Check them, then press Install.", Theme.Muted);
            if (root.TryGetProperty("notes", out var notes) && notes.ValueKind == JsonValueKind.Array)
                foreach (var nt in notes.EnumerateArray())
                    if (nt.ValueKind == JsonValueKind.String && nt.GetString() != "") Log("note: " + nt.GetString());
            StartPrecheck();       // a dump folder or .nsz packages change the disk space needed
        }

        void ApplyPrecheck()
        {
            string raw = _scanPre!.Output;
            var res = new List<Check>();
            try
            {
                int i = raw.IndexOf('[');
                if (i >= 0)
                {
                    using var doc = JsonDocument.Parse(raw[i..]);
                    foreach (var c in doc.RootElement.EnumerateArray())
                    {
                        res.Add(new Check { Level = Str(c, "level"), What = Str(c, "what"), Msg = Str(c, "msg") });
                        if (Str(c, "what") == "Disk space" && Num(c, "build_gb") is double bg)
                        {
                            _buildGb = bg;
                            _dumpGb = Num(c, "dump_gb") ?? _dumpGb;
                        }
                        var m = Regex.Match(Str(c, "msg"), @"--jobs (\d+)");
                        if (Str(c, "what") == "Memory" && m.Success && int.TryParse(m.Groups[1].Value, out int j) && j >= 1 && j <= 64)
                        {
                            _jobsSafe = j;
                            _jobs = j;
                        }
                    }
                }
            }
            catch { res.Clear(); }
            if (res.Count == 0)
            {
                _checks = new List<Check> { new() { Level = "warn", What = "Checks", Msg = $"The PC check did not run (code {_scanPre.ExitCode})." } };
                return;
            }
            _checks = res;
            UpdateLocStatus();
            _dirty = true;
        }

        // ---------------------------------------------------------------------------
        // Steps and progress (install.py events, or setup.py's plain lines as a fallback)
        void ResetSteps()
        {
            _steps.Clear();
            _jsonSeen = false; _fallbackN = 0;
            _errMsg = _errHint = _errAction = _doneMsg = "";
            _doneOk = null;
        }

        Step AddStep(string id, string title, double est)
        {
            var s = new Step { Id = id, Title = title, Est = est > 0 ? est : 10 };
            _steps.Add(s);
            return s;
        }

        Step? GetStep(string id) => _steps.FirstOrDefault(s => s.Id == id);

        void SetStepState(Step s, string state, double? secs, string msg)
        {
            s.State = state;
            if (state == "run" && s.T0 == null) s.T0 = DateTime.Now;
            if (secs is double v && v > 0) s.Secs = v;
            else if ((state is "done" or "fail") && s.T0 != null && s.Secs <= 0) s.Secs = (DateTime.Now - s.T0.Value).TotalSeconds;
            if (msg != "") s.Msg = msg;
            if (state == "done" && s.Total > 0) s.Done = s.Total;
        }

        static double Frac(Step s)
        {
            if (s.Total > 0) return Math.Clamp(s.Done / s.Total, 0, 1);
            if (s.T0 != null) return Math.Min(0.95, (DateTime.Now - s.T0.Value).TotalSeconds / s.Est);
            return 0;
        }

        // Overall fraction and time left, corrected by real/estimated time of the steps already finished.
        (double Frac, double Left) Overall()
        {
            double tot = 0, done = 0, rs = 0, re = 0, wait = 0;
            foreach (var s in _steps)
            {
                tot += s.Est;
                if (s.State is "done" or "skip" or "dry")
                {
                    done += s.Est;
                    if (s.State == "done" && s.Secs > 0) { rs += s.Secs; re += s.Est; }
                }
                else if (s.State is "run" or "fail") done += s.Est * Frac(s);
                else wait += s.Est;
            }
            double ratio = re > 0 ? Math.Clamp(rs / re, 0.25, 5.0) : 1.0;
            double left = wait * ratio;
            foreach (var s in _steps.Where(s => s.State == "run"))
            {
                double f = Frac(s);
                double el = s.T0 != null ? (DateTime.Now - s.T0.Value).TotalSeconds : 0;
                if (s.Total > 0 && f >= 0.03 && el >= 15) left += el * (1 - f) / f;
                else left += s.Est * (1 - f) * ratio;
            }
            return (tot > 0 ? Math.Min(1.0, done / tot) : 0, left);
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

        // A heading is the whole title after "[4/9]", not its first word. Seeing it means that
        // step is the one running and the ones before it have finished.
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

        // Block checks, each module's C ("name: 8 files, 37 MB of C"), and the build's own bar.
        // That bar's percent is a time estimate, and writing C is a small part of it, so a line
        // that names "N of M modules" moves the bar by modules instead of sitting near 0%.
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

        void HandleLine(string line)
        {
            if (line.Trim() == "SHADER_PRIORITY?")
            {
                Log("Choose how hard this PC works on the shaders.");
                SetStatus("Choose a shader priority.", Theme.Warn);
                _modal = new Prompt
                {
                    Title = "SHADER COMPILE",
                    Body = "Shaders compile in this window, and the bar shows how far along they are. Low keeps the PC easy to use. Medium is in between. High finishes sooner, and the PC feels slower until it is done.",
                    Yes = "HIGH",
                    Mid = "MEDIUM",
                    No = "LOW",
                    OnYes = () => _job?.WriteLine("high"),
                    OnMid = () => _job?.WriteLine("medium"),
                    OnNo = () => _job?.WriteLine("low"),
                };
                _dirty = true;
                return;
            }
            if (line.StartsWith("##ssbu "))
            {
                string json = line[7..];
                Log("event " + json, toScreen: false);
                try
                {
                    using var doc = JsonDocument.Parse(json);
                    HandleEvent(doc.RootElement);
                }
                catch { Log("(unreadable event) " + json); }
                return;
            }
            Log(line);
            if (!_jsonSeen)
            {
                string trimmed = line.Trim();
                if (ParseSetupLine(trimmed)) { }
                else if (Regex.IsMatch(trimmed, @"^FAILED at step (\w+)"))
                {
                    foreach (var s in _steps.Where(s => s.State == "run")) SetStepState(s, "fail", null, "");
                    if (_errMsg == "") _errMsg = line;
                }
                else ApplyMeasure(trimmed);
            }
            if (line.Length > 0 && !line.StartsWith("##")) SetStatus(line, Theme.Muted);
            if (!_licenseAsked && line.Contains("Type yes and press Enter to accept"))
            {
                _licenseAsked = true;
                string url = "https://go.microsoft.com/fwlink/?LinkId=2327714";
                for (int i = _log.Count - 1; i >= 0 && i >= _log.Count - 12; i--)
                {
                    int h = _log[i].IndexOf("https://", StringComparison.Ordinal);
                    if (h >= 0) { url = _log[i][h..].Trim(); break; }
                }
                SetStatus("Accept Microsoft's license terms to continue.", Theme.Warn);
                Confirm(
                    "ACCEPT MICROSOFT'S LICENSE?",
                    "The compiler needs Microsoft's C runtime and the Windows SDK (about 570 MB). They come from Microsoft. Yes accepts the Visual Studio Build Tools license terms. Read them first:\n" + url,
                    "YES",
                    () => AnswerCompilerLicense(true),
                    () => AnswerCompilerLicense(false));
            }
        }

        // ---------------------------------------------------------------------------
        // Jobs
        List<string> BuildInstallArgs(bool dry)
        {
            var a = new List<string> { "-u", Script("install.py"), "--no-gui", "--progress-json" };
            string b = Val("base");
            bool isDump = IsDir(b);
            if (b != "")
            {
                if (isDump) { a.Add("--dump"); a.Add(b.TrimEnd('\\')); }
                else { a.Add("--base"); a.Add(b); }
            }
            if (!isDump)
            {
                if (Val("update") != "") { a.Add("--update"); a.Add(Val("update")); }
                foreach (var d in _dlc) { a.Add("--dlc"); a.Add(d); }
                if (Val("prodkeys") != "") { a.Add("--keys"); a.Add(Val("prodkeys")); }
            }
            if (Val("firmware") != "") { a.Add("--firmware"); a.Add(Val("firmware").TrimEnd('\\')); }
            // The install location: passed when it is not the default, or to go back to the default
            // (install.py then forgets the old one).
            string loc = GetChosenBuild();
            if (!SamePath(loc, _defaultBuild) || _storedBuild != "") { a.Add("--build-dir"); a.Add(loc); }
            a.Add("--jobs"); a.Add(_jobs.ToString(CultureInfo.InvariantCulture));
            if (dry) a.Add("--dry-run");
            if (!_optRuntime) a.Add("--no-runtime");
            if (!_optShortcuts) a.Add("--no-shortcuts");
            if (_optFast) a.Add("--no-inline");
            if (!_optUpdates) a.Add("--no-self-update");
            return a;
        }

        // Each stage may ask the user first; "Yes" continues with the next stage.
        void StartInstall(bool dry)
        {
            if (_running) return;
            CheckLocal();
            var bad = new List<string>();
            if (_keysOk != true) bad.Add("Keys");
            if (_fwOk != true) bad.Add("Firmware");
            if (_gameOk != true) bad.Add("Game files");
            if (!dry && bad.Count > 0)
            {
                Alert("FIX THESE FOLDERS", "Please fix: " + string.Join(", ", bad) + ".\n\nThe line under each folder says what is wrong. Press Check files after you change one.");
                return;
            }
            string ps1 = Path.Combine(_repo, "tools", "install_windowsmash.ps1");
            if (!File.Exists(ps1))
            {
                Alert("INSTALLER NOT FOUND", "tools\\install_windowsmash.ps1 is not in this folder. This copy of OpenSmash is incomplete.");
                return;
            }
            if (string.IsNullOrWhiteSpace(_onlineName))
            {
                Alert("TYPE A USERNAME", "Type a username before the build can start.");
                return;
            }
            string root = PsQuote(_repo);
            var cmd = new StringBuilder();
            cmd.Append("Get-ChildItem -LiteralPath '").Append(root).Append("' -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue; ");
            cmd.Append("& '").Append(root).Append("\\tools\\install_windowsmash.ps1'");
            cmd.Append(" -NoWizard -Game '").Append(PsQuote(_gamePath)).Append("' -Keys '").Append(PsQuote(_keysPath)).Append("'");
            if (string.IsNullOrWhiteSpace(_fwPath)) cmd.Append(" -NoFirmware");
            else cmd.Append(" -Firmware '").Append(PsQuote(_fwPath)).Append("'");
            cmd.Append(" -OnlineName '").Append(PsQuote(_onlineName.Trim())).Append("'");
            if (_optShortcuts) cmd.Append(" -Shortcut");
            if (_optFirewall) cmd.Append(" -Firewall");
            if (_optShaders) cmd.Append(" -Shaders");
            if (dry) cmd.Append(" -DryRun");
            if (!_optUpdates) cmd.Append(" -NoUpdate");
            if (_optLicense) cmd.Append(" -AcceptLicense");
            if (_jobs > 0) cmd.Append(" -Jobs ").Append(_jobs.ToString(CultureInfo.InvariantCulture));
            if (_nameBox != null) _nameBox.Visible = false;
            RunJob("install", dry, "powershell.exe",
                new List<string> { "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", cmd.ToString() });
        }

        static string PsQuote(string s) => s.Replace("'", "''");

        void StartUpdate()
        {
            if (_running) return;
            _optUpdates = true;
            StartInstall(_optDry);
        }

        void StartBootstrap()
        {
            if (_running) return;
            string ps = Script("bootstrap.ps1");
            if (!File.Exists(ps))
            {
                _view = "build";
                _jobKind = "bootstrap";
                ResetSteps();
                _errMsg = "scripts\\bootstrap.ps1 was not found.";
                _errHint = "This copy of OpenSmash is incomplete: download it again.";
                _dirty = true;
                return;
            }
            RunJob("bootstrap", false, "powershell.exe",
                new List<string> { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps, "-NoLaunch" });
        }

        void RunJob(string kind, bool dry, string exe, List<string> args)
        {
            _jobKind = kind; _jobDry = dry;
            _running = true; _succeeded = false;
            _jobStart = DateTime.Now;
            _logScroll = 0;
            ResetSteps();
            if (kind == "update") AddStep("update", "Update from GitHub", 15);
            if (kind == "bootstrap") AddStep("python", "Set up Python for the installer", 120);
            Log($"=== {DateTime.Now:yyyy-MM-dd HH:mm:ss} {kind}: {exe} {string.Join(" ", args)}", toScreen: false);
            Log("Starting " + kind + (dry ? " (dry run)" : "") + " \u2026");
            _job = new Proc(capture: false);
            if (!_job.Start(exe, args, _repo))
            {
                Log("Could not start: " + _job.StartError);
                _errMsg = "Could not start " + Path.GetFileName(exe) + ": " + _job.StartError;
            }
            else if (kind is "update" or "bootstrap") SetStepState(_steps[0], "run", null, "");
            _view = "build";
            SetStatus(kind == "bootstrap"
                ? "Setting up Python for the installer (one time, about 100 MB)\u2026"
                : "Working\u2026 this can take from 30 minutes to a few hours. If you close the window, the next start goes on from where it stopped.", Theme.Muted);
        }

        void FinishJob(int code)
        {
            _running = false;
            _jobEnd = DateTime.Now;
            bool ok = code == 0 && _doneOk != false && _errMsg == "";
            if (ok && _jobKind == "bootstrap")
            {
                _py = ResolvePython();
                if (_py == "")
                {
                    ok = false;
                    _errMsg = "Python could not be set up.";
                    _errHint = "Check your internet connection, then press Retry.";
                }
            }

            if (ok)
            {
                foreach (var s in _steps)
                {
                    if (s.State == "run") SetStepState(s, "done", null, "");
                    else if (s.State == "wait") SetStepState(s, _jobDry ? "dry" : "skip", null, "");
                }
                _succeeded = true;
                _failedBefore = false;
                string msg = _doneMsg != "" ? _doneMsg
                    : _jobDry ? "Dry run finished: nothing was changed."
                    : _jobKind == "update" ? "Update finished."
                    : _jobKind == "bootstrap" ? "Python is ready."
                    : "Done. The game is built.";
                SetStatus(char.ToUpperInvariant(msg[0]) + msg[1..], Theme.Ready);
            }
            else
            {
                foreach (var s in _steps.Where(s => s.State == "run")) SetStepState(s, "fail", null, "");
                _failedBefore = _jobKind == "install";
                if (_errMsg == "") _errMsg = $"It stopped (code {code}). The log below says why.";
                if (_errHint == "")
                    _errHint = _jobKind == "install"
                        ? "Fix the problem, then press Resume: finished steps are not repeated."
                        : "Fix the problem, then press Retry.";
                SetStatus("Stopped.", Theme.Bad);
            }

            if (_jobKind == "install" && !_jobDry)
            {
                if (ok) Installed = WindowSmash.IsBuilt(_repo);
            }
            if (_jobKind == "bootstrap" && ok)
            {
                _view = "setup";
                StartScans();
            }
            Log($"=== finished, exit code {code}, {(ok ? "ok" : "not ok")}", toScreen: false);
            _dirty = true;
        }

        void StopJob()
        {
            Confirm("STOP THE BUILD?", "The next start goes on from where it stopped: finished steps are not repeated.", "STOP", () =>
            {
                _job?.Kill();      // only the process tree this window started
                Log("Stopped by the user.");
            });
        }

        void OpenLogs()
        {
            string log = Path.Combine(_repo, "install.log");
            try
            {
                string args = File.Exists(log) ? "/select,\"" + log + "\"" : "\"" + _repo + "\"";
                Process.Start(new ProcessStartInfo("explorer.exe", args) { UseShellExecute = true });
            }
            catch { }
        }

        // The MSVC Build Tools in a console the user can see (winget's own progress and Microsoft's installer window).
        void InstallBuildTools()
        {
            const string cmd = "winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override \"--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"";
            Confirm("INSTALL THE C++ BUILD TOOLS?",
                "Free, about 3 GB on C:, 5-15 minutes. Yes accepts the Microsoft licence terms of the Build Tools and of the winget source; Windows asks for administrator rights.\n\nA console window shows what happens. When it says it is finished, close it and press Resume here.",
                "INSTALL", () =>
                {
                    try
                    {
                        Process.Start(new ProcessStartInfo("cmd.exe",
                            "/k echo Installing the Microsoft C++ Build Tools (5-15 minutes; a Microsoft window shows the progress)... & " + cmd +
                            " & echo. & echo Finished: read the lines above. Close this window and press Resume in the installer.") { UseShellExecute = true });
                        Log("started in a console: " + cmd);
                    }
                    catch (Exception ex) { Alert("COULD NOT START", "The console could not start: " + ex.Message); }
                });
        }

        void Resume()
        {
            if (_jobKind == "bootstrap") StartBootstrap();
            else if (_jobKind == "update") StartUpdate();
            else StartInstall(_jobDry);
        }

        // ---------------------------------------------------------------------------
        // Log, status, modal
        void Log(string line, bool toScreen = true)
        {
            if (toScreen)
            {
                _log.Add(line);
                if (_log.Count > 4000) _log.RemoveRange(0, 1000);
            }
            _filePending.Append(DateTime.Now.ToString("HH:mm:ss")).Append("  ").AppendLine(line);
            _dirty = true;
        }

        void FlushLogFile()
        {
            if (_filePending.Length == 0) return;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(_logFile)!);
                File.AppendAllText(_logFile, _filePending.ToString(), new UTF8Encoding(false));
            }
            catch { }
            _filePending.Clear();
        }

        void SetStatus(string text, Color color)
        {
            _status = text;
            _statusColor = color;
            _dirty = true;
        }

        void Alert(string title, string body) => _modal = new Prompt { Title = title, Body = body, Yes = "OK" };

        void Confirm(string title, string body, string yes, Action onYes, Action? onNo = null) =>
            _modal = new Prompt { Title = title, Body = body, Yes = yes, No = "CANCEL", OnYes = onYes, OnNo = onNo };

        void AnswerCompilerLicense(bool accept)
        {
            try
            {
                string dir = Path.Combine(_repo, "deps");
                Directory.CreateDirectory(dir);
                File.WriteAllText(Path.Combine(dir, ".msvc-license-answer"), accept ? "yes\n" : "no\n");
            }
            catch (Exception ex) { Log("Could not answer the license prompt: " + ex.Message); }
            try { _job?.WriteLine(accept ? "yes" : "no"); } catch { }
        }

        // ---------------------------------------------------------------------------
        // Timer: drains process output, picks up finished scans, keeps the clock moving
        void OnTick()
        {
            _tick++;
            if (_job != null)
            {
                int n = 0;
                while (n < 400 && _job.Lines.TryDequeue(out var line)) { n++; HandleLine(line); }
                if (_running && _job.Done && _job.Lines.IsEmpty) FinishJob(_job.StartError != "" ? 1 : _job.ExitCode);
            }
            if (_scanFind is { Done: true } && !_foundDone) { _foundDone = true; ApplyFound(); }
            if (_scanPre is { Done: true } && !_preDone)
            {
                _preDone = true;
                ApplyPrecheck();
                if (_preAgain) { _preAgain = false; StartPrecheck(); }
            }
            if (_paramsChangedAt is DateTime t && !_running && (DateTime.Now - t).TotalSeconds >= 1.5)
            {
                _paramsChangedAt = null;
                StartPrecheck();
            }
            if (_tick % 50 == 0 && !_running && _view == "setup") { UpdateLocStatus(); _dirty = true; }  // free space, live
            if (_tick % 10 == 0) FlushLogFile();
            if (_dirty || (_running && _tick % 5 == 0))
            {
                _dirty = false;
                Invalidate();
            }
        }

        void OnFormClosing(object? sender, FormClosingEventArgs e)
        {
            if (_running && _job != null && !_job.Done && !_closeConfirmed)
            {
                e.Cancel = true;
                Confirm("STOP AND CLOSE?", "The build is running. If you close the window it stops; the next start goes on from where it stopped.", "STOP AND CLOSE", () =>
                {
                    _closeConfirmed = true;
                    _job.Kill();
                    Log("Stopped by the user (window closed).");
                    Close();
                });
                _dirty = true;
                return;
            }
            _scanFind?.Kill();
            _scanPre?.Kill();
            _timer.Stop();
            FlushLogFile();
            if (Installed) DialogResult = DialogResult.OK;
        }

        // ---------------------------------------------------------------------------
        // Painting
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
            _tips.Clear();
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

            if (_nameBox != null && _view != "setup") _nameBox.Visible = false;
            if (_view == "build") PaintBuild(g, W, H);
            else PaintSetup(g, W, H);

            PaintFooter(g, W, H);
            Theme.CloseButton(g, W, _s, _hover == "close", out var close);
            _hits.Add(("close", close));

            if (_modal != null) PaintModal(g, W, H);
        }

        void PaintHeader(Graphics g, int W, string eyebrow, string title, string subtitle)
        {
            float s = _s, left = 96 * s;

            // Eyebrow: a red skewed dash and mono red caps, as on the launcher's pages
            float ey = 34 * s, eh = 16 * s, k = 2 * s * 0.577f;
            using (var b = new SolidBrush(Theme.Accent))
                g.FillPolygon(b, new[] { new PointF(left + k, ey + eh / 2 - s), new PointF(left + 34 * s + k, ey + eh / 2 - s), new PointF(left + 34 * s, ey + eh / 2 + s), new PointF(left, ey + eh / 2 + s) });
            using (var f = Theme.Mono(13 * s))
                Theme.Label(g, eyebrow, f, Theme.Accent, left + 46 * s, ey, eh, 13 * s * 0.2f);

            // Title with the red period
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

        // .road-item hover: a red wash that grows in from the left, with a solid edge
        void RowWash(Graphics g, float x, float y, float w, float h, int alpha = 36)
        {
            using (var lg = new LinearGradientBrush(new RectangleF(x - 1, y, w + 2, h), Color.FromArgb(alpha, Theme.Accent), Color.FromArgb(0, Theme.Accent), LinearGradientMode.Horizontal))
                g.FillRectangle(lg, x, y, w, h);
            using (var b = new SolidBrush(Theme.Accent))
                g.FillRectangle(b, x, y, 3 * _s, h);
        }

        // The kicker's small slanted square, used as a status dot
        void StatusMark(Graphics g, Color c, float x, float cy)
        {
            float s = _s;
            using var b = new SolidBrush(c);
            g.FillPolygon(b, new[] { new PointF(x + 2 * s, cy - 3 * s), new PointF(x + 8 * s, cy - 3 * s), new PointF(x + 6 * s, cy + 3 * s), new PointF(x, cy + 3 * s) });
        }

        // Small pill button; hit id = id.
        void Pill(Graphics g, string id, string label, float x, float y, float w, float h, bool enabled = true)
        {
            Theme.Button(g, label, x, y, w, h, _s, primary: false, hot: enabled && _hover == id, enabled, 11 * _s);
            if (enabled) Hit(id, x, y, w, h);
        }

        // Primary / secondary action buttons along the bottom.
        float ActionButton(Graphics g, string id, string label, float x, float y, bool primary, bool enabled = true)
        {
            float s = _s;
            float w, h;
            if (primary) { w = 230 * s; h = 46 * s; }
            else
            {
                using var f = Theme.Body(13 * s, true);
                w = Math.Max(124 * s, g.MeasureString(label, f).Width + 34 * s);
                h = 36 * s;
                y += 5 * s;
            }
            Theme.Button(g, label, x, y, w, h, s, primary, enabled && _hover == id, enabled);
            if (enabled) Hit(id, x, y, w, h);
            return x + w + 12 * s;
        }

        // ---- the folders the WindowSmash setup window asked for, drawn in this screen's style
        void DetectFolders()
        {
            var saved = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            string savedFile = Path.Combine(_repo, "dump", "build_folders.txt");
            if (File.Exists(savedFile))
            {
                foreach (string line in File.ReadAllLines(savedFile))
                {
                    int i = line.IndexOf('=');
                    if (i > 0) saved[line[..i]] = line[(i + 1)..];
                }
            }
            string app = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var keys = new List<string>();
            var firmware = new List<string>();
            var game = new List<string>();
            foreach (string name in new[] { "yuzu", "eden", "sudachi", "citron", "suyu" })
            {
                keys.Add(Path.Combine(app, name, "keys"));
                firmware.Add(Path.Combine(app, name, "nand", "system", "Contents", "registered"));
                game.Add(Path.Combine(app, name, "nand", "user", "Contents", "registered"));
                game.Add(Path.Combine(app, name, "sdmc", "Nintendo", "Contents", "registered"));
            }
            keys.Add(Path.Combine(app, "Ryujinx", "system"));
            firmware.Add(Path.Combine(app, "Ryujinx", "bis", "system", "Contents", "registered"));
            game.Add(Path.Combine(app, "Ryujinx", "bis", "user", "Contents", "registered"));
            game.Add(Path.Combine(app, "Ryujinx", "sdcard", "Nintendo", "Contents", "registered"));
            keys.Add(Path.Combine(home, ".switch"));
            _gamePath = saved.TryGetValue("game", out string? sg) ? sg : game.FirstOrDefault(Directory.Exists) ?? "";
            _keysPath = saved.TryGetValue("keys", out string? sk) ? sk : keys.FirstOrDefault(p => File.Exists(Path.Combine(p, "prod.keys"))) ?? "";
            _fwPath = saved.TryGetValue("firmware", out string? sf) ? sf : firmware.FirstOrDefault(p => Directory.Exists(p) && Directory.EnumerateFiles(p, "*.nca").Any()) ?? "";
            SetStatus("Folders found on this PC are filled in. Change any of them with Browse.", Theme.Muted);
        }

        void CheckLocal()
        {
            if (!Directory.Exists(_keysPath)) { _keysOk = false; _keysStatus = "Choose the folder with prod.keys and title.keys."; }
            else if (!File.Exists(Path.Combine(_keysPath, "prod.keys"))) { _keysOk = false; _keysStatus = "There is no prod.keys here."; }
            else if (!File.Exists(Path.Combine(_keysPath, "title.keys"))) { _keysOk = true; _keysStatus = "prod.keys found; no title.keys (needed for games bought from the eShop)."; }
            else { _keysOk = true; _keysStatus = "prod.keys and title.keys found."; }

            if (string.IsNullOrWhiteSpace(_fwPath)) { _fwOk = true; _fwStatus = "None: the game uses the open fonts, without Chinese, Japanese or Korean text. Your firmware gives your console's own fonts."; }
            else if (!Directory.Exists(_fwPath)) { _fwOk = false; _fwStatus = "Choose the folder with your firmware (a few hundred .nca files), or leave this empty."; }
            else
            {
                int n = 0;
                try { n = Directory.EnumerateFiles(_fwPath, "*.nca").Count(); } catch { }
                if (n == 0) { _fwOk = false; _fwStatus = "There are no .nca files here."; }
                else { _fwOk = true; _fwStatus = n + " .nca files (the game's fonts are taken out of them)."; }
            }
            if (!Directory.Exists(_gamePath)) { _gameOk = false; _gameStatus = "Choose the folder with your game files."; }
            else if (_keysOk != true) { _gameOk = false; _gameStatus = "The keys folder is needed to read the game files."; }
        }

        async Task CheckGameAsync()
        {
            int gen = ++_scanGen;
            string folder = _gamePath, keys = _keysPath;
            if (!Directory.Exists(folder) || _keysOk != true)
            {
                _dirty = true;
                return;
            }
            _gameStatus = "Checking…";
            _gameOk = null;
            _dirty = true;
            var found = await Task.Run(() => ScanGame(folder, keys));
            if (gen != _scanGen || IsDisposed) return;
            _gameOk = found.Ok;
            _gameStatus = found.Text;
            _dirty = true;
            Invalidate();
        }

        (bool? Ok, string Text) ScanGame(string folder, string keys)
        {
            if (File.Exists(Path.Combine(folder, "romfs.bin")))
                return (true, "romfs.bin found (the game data, used as it is).");
            string? py = PythonExe();
            string script = Path.Combine(_repo, "tools", "gamefiles.py");
            if (py == null || !File.Exists(script))
                return (true, "This folder will be read after Python is installed.");
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = py,
                    Arguments = py.EndsWith("py.exe", StringComparison.OrdinalIgnoreCase) || Path.GetFileName(py).Equals("py", StringComparison.OrdinalIgnoreCase)
                        ? "-3 \"" + script + "\" \"" + folder + "\" --keys \"" + keys + "\" --json"
                        : "\"" + script + "\" \"" + folder + "\" --keys \"" + keys + "\" --json",
                    WorkingDirectory = _repo,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true,
                };
                using var p = Process.Start(psi);
                if (p == null) return (false, "Could not read the folder.");
                string output = p.StandardOutput.ReadToEnd();
                p.WaitForExit(120000);
                using var doc = JsonDocument.Parse(output);
                var root = doc.RootElement;
                var lines = new List<string>();
                if (root.TryGetProperty("summary", out var summary) && summary.ValueKind == JsonValueKind.Array)
                    foreach (var item in summary.EnumerateArray()) lines.Add(item.GetString() ?? "");
                if (File.Exists(Path.Combine(folder, "romfs.bin"))) lines.Add("romfs.bin found (the game data, used as it is).");
                bool program = root.TryGetProperty("program", out var prog) && prog.ValueKind is not JsonValueKind.Null and not JsonValueKind.Undefined;
                string text = string.Join(" ", lines.Where(s => s.Length > 0));
                if (program) return (true, text.Length > 0 ? text : "Game files found.");
                string problem = root.TryGetProperty("problem", out var pr) && pr.ValueKind == JsonValueKind.String ? pr.GetString() ?? "the game was not found" : "the game was not found";
                return (false, ("Problem: " + problem + " " + text).Trim());
            }
            catch (Exception ex)
            {
                return (false, "Could not read the folder: " + ex.Message);
            }
        }

        string? PythonExe()
        {
            string emb = Path.Combine(_repo, "deps", "python", "python.exe");
            if (File.Exists(emb)) return emb;
            string? path = Environment.GetEnvironmentVariable("PATH");
            if (path == null) return null;
            foreach (string dir in path.Split(Path.PathSeparator))
            {
                string py = Path.Combine(dir, "py.exe");
                if (File.Exists(py)) return py;
                string python = Path.Combine(dir, "python.exe");
                if (File.Exists(python)) return python;
            }
            return null;
        }

        void PickFolder(string which)
        {
            using var dlg = new FolderBrowserDialog { Description = "Choose the folder", UseDescriptionForTitle = true };
            string cur = which switch { "keys" => _keysPath, "firmware" => _fwPath, _ => _gamePath };
            if (Directory.Exists(cur)) dlg.SelectedPath = cur;
            if (dlg.ShowDialog(this) != DialogResult.OK) return;
            if (which == "keys") _keysPath = dlg.SelectedPath;
            else if (which == "firmware") _fwPath = dlg.SelectedPath;
            else _gamePath = dlg.SelectedPath;
            CheckLocal();
            _ = CheckGameAsync();
        }

        // ---- setup view: the WindowSmash questions, in this window's cards
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
            using (var f = Theme.Body(16 * s, true))
                Theme.DrawText(g, "Username", f, Theme.Text, left + 4 * s, nameY, 108 * s, 36 * s);
            if (_nameBox != null)
            {
                _nameBox.Visible = _modal == null;
                _nameBox.SetBounds((int)(left + 118 * s), (int)nameY, (int)(leftW - 122 * s), (int)(36 * s));
            }

            SectionLabel(g, "OPTIONS", rx, 138 * s);
            float oy = 158 * s;
            float cardH = 392 * s;
            Theme.Card(g, rx, oy, rightW, cardH, 10 * s, s);
            float oh = 52 * s;
            oy += 10 * s;
            PaintOption(g, "opt:shortcuts", "Make a desktop shortcut", _optShortcuts, rx, oy, rightW, 36 * s, "A desktop shortcut that starts OpenSmash."); oy += 36 * s;
            PaintOption(g, "opt:firewall", "Allow the game through Windows Firewall now (asks for administrator rights once; needed for playing online)", _optFirewall, rx, oy, rightW, oh, "Needed for playing online. Asks for administrator rights once."); oy += oh;
            PaintOption(g, "opt:shaders", "After the build, prepare the shaders in this window (you pick low, medium, or high; stops the first-time stutter)", _optShaders, rx, oy, rightW, oh, "Stops the first-time stutter of effects. Progress shows in this window."); oy += oh + 4 * s;
            using (var f = Theme.Body(12 * s))
                Theme.DrawWrapped(g, "Then the game is built, which takes 30 minutes to a few hours. If it stops, run it again: it carries on where it stopped.", f, Theme.Dim, rx + 16 * s, oy, rightW - 32 * s, 48 * s);
            oy += 54 * s;
            using (var pen = new Pen(Theme.Line, Math.Max(1, s)))
                g.DrawLine(pen, rx + 16 * s, oy, rx + rightW - 16 * s, oy);
            oy += 10 * s;
            const string jobsTip = "Compilers running at once. Fewer leaves the PC usable.";
            using (var f = Theme.Body(12.5f * s))
                Theme.DrawText(g, "Compiler jobs", f, Theme.Text, rx + 16 * s, oy, 120 * s, 28 * s);
            float jx = rx + 140 * s;
            Pill(g, "jobs:-", "\u2212", jx, oy + 1 * s, 26 * s, 26 * s, _jobs > 1);
            using (var f = Theme.Body(13 * s, true))
                Theme.DrawText(g, _jobs.ToString(CultureInfo.InvariantCulture), f, Theme.Text, jx + 26 * s, oy, 36 * s, 28 * s, StringAlignment.Center);
            Pill(g, "jobs:+", "+", jx + 62 * s, oy + 1 * s, 26 * s, 26 * s, _jobs < 64);
            using (var f = Theme.Body(11.5f * s))
                Theme.DrawText(g, "fewer = PC stays usable", f, Theme.Dim, jx + 98 * s, oy, rightW - (jx - rx) - 110 * s, 28 * s);
            _tips["jobs:-"] = _tips["jobs:+"] = jobsTip;
            PaintOption(g, "opt:updates", "Check for updates first", _optUpdates, rx, oy + 32 * s, rightW, 32 * s, "Looks for a newer WindowSmash on GitHub before building.");
            PaintOption(g, "opt:license", "Accept Microsoft's license terms for the C runtime and Windows SDK (the compiler download, about 570 MB)", _optLicense, rx, oy + 64 * s, rightW, 72 * s, "Read them first: https://go.microsoft.com/fwlink/?LinkId=2327714");

            float ay = 574 * s;
            string label = _failedBefore || WindowSmash.IsBuilt(_repo) ? "RESUME"
                : _succeeded && _jobKind == "install" && !_jobDry ? "BUILD AGAIN"
                : "SET UP AND BUILD";
            float ax = ActionButton(g, "act:check", "CHECK FILES", left, ay, primary: false);
            ax = ActionButton(g, "act:install", label, ax, ay, primary: true);
            ActionButton(g, "act:logs", "OPEN LOGS", ax, ay, false);
        }

        float PaintPathRow(Graphics g, string id, string title, string hint, string path, string status, bool? ok, float x, float y, float w)
        {
            float s = _s, h = 112 * s, gap = 8 * s;
            Theme.Card(g, x, y, w, h - gap, 10 * s, s);
            bool hot = _hover == "pick:" + id || _hover == "clear:" + id;
            if (hot) RowWash(g, x, y, w, h - gap, 22);
            using (var f = Theme.Display(16 * s))
                Theme.Label(g, title, f, Theme.Text, x + 14 * s, y + 8 * s, 18 * s, 0.16f * s);
            using (var f = Theme.Body(11.5f * s))
                Theme.DrawText(g, hint, f, Theme.Muted, x + 14 * s, y + 28 * s, w - 28 * s, 16 * s);
            string shown = string.IsNullOrEmpty(path) ? "Not chosen" : path;
            float bw = 78 * s, bh = 26 * s;
            int buttons = id == "firmware" && !string.IsNullOrEmpty(path) ? 2 : 1;
            float bx = x + w - 12 * s - buttons * bw - (buttons - 1) * 8 * s;
            using (var f = Theme.Body(12.5f * s))
            {
                if (string.IsNullOrEmpty(path))
                    Theme.DrawText(g, shown, f, Theme.Dim, x + 14 * s, y + 46 * s, bx - x - 22 * s, 22 * s);
                else
                    Theme.DrawPath(g, shown, f, Theme.Text, x + 14 * s, y + 46 * s, bx - x - 22 * s, 22 * s);
            }
            if (buttons == 2) Pill(g, "clear:" + id, "CLEAR", bx, y + 44 * s, bw, bh);
            Pill(g, "pick:" + id, "BROWSE", x + w - 12 * s - bw, y + 44 * s, bw, bh);
            Color sc = ok == true ? Theme.Ready : ok == false ? Theme.Bad : Theme.Dim;
            StatusMark(g, sc, x + 16 * s, y + 84 * s);
            using (var f = Theme.Body(11.5f * s))
                Theme.DrawWrapped(g, status, f, sc, x + 30 * s, y + 72 * s, w - 44 * s, 30 * s);
            return y + h;
        }

        void PaintFieldRow(Graphics g, string key, float x, float y, float w, float h)
        {
            float s = _s;
            var f = _f[key];
            string[] hv = _hover.Split(':');
            bool hot = hv.Length > 1 && hv[1] == key;

            if (hot) RowWash(g, x, y, w, h);
            using (var pen = new Pen(Theme.Line, Math.Max(1, s)))
                g.DrawLine(pen, x, y + h, x + w, y + h);

            Hit("tip:" + key, x, y, w, h);
            _tips["tip:" + key] = f.Status != "" ? f.Status : f.Hint;

            float lx = x + (hot ? 24 : 16) * s;
            using (var ft = Theme.Display(22 * s))
                Theme.Label(g, f.Label, ft, Theme.Text, lx, y + 13 * s, 20 * s, 0.22f * s);
            string tag = key is "update" or "prodkeys" && IsDir(Val("base")) ? "NOT NEEDED" : f.Tag;
            using (var ft = Theme.Mono(10.5f * s, true))
                Theme.Label(g, tag, ft, tag == "REQUIRED" ? Theme.Accent : Theme.Dim, lx, y + 38 * s, 12 * s, 10.5f * s * 0.12f);

            // buttons, right aligned
            var buttons = key switch
            {
                "base" => new[] { ("pick:base:file", "FILE"), ("pick:base:folder", "FOLDER") },
                "dlc" => _dlc.Count > 0 ? new[] { ("pick:dlc", "ADD"), ("clear:dlc", "CLEAR") } : new[] { ("pick:dlc", "ADD") },
                "firmware" => Val("firmware") != "" ? new[] { ("pick:firmware", "BROWSE"), ("clear:firmware", "CLEAR") } : new[] { ("pick:firmware", "BROWSE") },
                "buildloc" => new[] { ("pick:buildloc", "CHOOSE"), ("default:buildloc", "DEFAULT") },
                _ => new[] { ($"pick:{key}", "BROWSE") },
            };
            float bw = 78 * s, bh = 30 * s, bgap = 8 * s;
            float bx = x + w - 14 * s - buttons.Length * bw - (buttons.Length - 1) * bgap;
            float vx = x + 172 * s, vw = bx - 14 * s - vx;
            for (int i = 0; i < buttons.Length; i++)
                Pill(g, buttons[i].Item1, buttons[i].Item2, bx + i * (bw + bgap), y + (h - bh) / 2, bw, bh, !_running);

            // value
            using (var fv = Theme.Body(13 * s))
            {
                if (key == "dlc")
                {
                    string t = _dlc.Count == 0 ? "None chosen" : _dlc.Count == 1 ? Path.GetFileName(_dlc[0]) : $"{_dlc.Count} packages";
                    Theme.DrawText(g, t, fv, _dlc.Count == 0 ? Theme.Dim : Theme.Text, vx, y + 10 * s, vw, 22 * s);
                }
                else if (Val(key) == "") Theme.DrawText(g, "Not chosen", fv, Theme.Dim, vx, y + 10 * s, vw, 22 * s);
                else Theme.DrawPath(g, Val(key), fv, Theme.Text, vx, y + 10 * s, vw, 22 * s);
            }
            // status line
            StatusMark(g, f.StatusColor, vx, y + 43 * s);
            using (var fs = Theme.Body(11.5f * s))
                Theme.DrawText(g, f.Status, fs, f.StatusColor == Theme.Dim ? Theme.Dim : Blend(f.StatusColor), vx + 14 * s, y + 34 * s, vw - 14 * s, 18 * s);
        }

        // Status colours read a little softer as body text than as dots.
        static Color Blend(Color c) => Color.FromArgb(255, (c.R * 3 + 245) / 4, (c.G * 3 + 245) / 4, (c.B * 3 + 245) / 4);

        void PaintOption(Graphics g, string id, string label, bool on, float x, float y, float w, float h, string tip)
        {
            float s = _s;
            bool hot = _hover == id;
            if (hot) RowWash(g, x + 1, y, w - 2, h, 28);
            float bs = 14 * s, bxx = x + 16 * s, byy = y + (h - bs) / 2;
            using (var p = Theme.Para(bxx, byy, bs + 3 * s, bs, 3 * s))
            {
                if (on)
                {
                    using (var b = new SolidBrush(Theme.Accent)) g.FillPath(b, p);
                    using var pen = new Pen(Color.White, Math.Max(1.4f, 1.8f * s)) { LineJoin = LineJoin.Miter };
                    float cx = bxx + 1.5f * s;
                    g.DrawLines(pen, new[] { new PointF(cx + bs * 0.24f, byy + bs * 0.52f), new PointF(cx + bs * 0.43f, byy + bs * 0.72f), new PointF(cx + bs * 0.77f, byy + bs * 0.30f) });
                }
                else
                {
                    using var pen = new Pen(hot ? Theme.Text : Theme.Dim, Math.Max(1, s));
                    g.DrawPath(pen, p);
                }
            }
            using (var f = Theme.Body(12.5f * s))
                Theme.DrawWrapped(g, label, f, on ? Theme.Text : Theme.Muted, x + 40 * s, y + 2 * s, w - 52 * s, h - 4 * s);
            Hit(id, x, y, w, h);
            _tips[id] = tip;
        }

        // ---- build view: steps, progress, log
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
                using (var f = Theme.Body(12 * s))
                    Theme.DrawText(g, $"\u2026and {hidden} more", f, Theme.Dim, left + 30 * s, y0 + visible * rowH, lw - 40 * s, rowH);
            if (_steps.Count == 0)
                using (var f = Theme.Body(13 * s))
                    Theme.DrawText(g, _running ? "Waiting for the first step\u2026" : "No steps ran.", f, Theme.Dim, left + 4 * s, y0, lw, rowH);
            for (int i = first; i < Math.Min(_steps.Count, first + visible); i++)
            {
                var st = _steps[i];
                float ry = y0 + (i - first) * rowH;
                if (st.State == "run") Theme.FillAccentBar(g, left, ry + 1 * s, lw, rowH - 2 * s, 6 * s, 90);
                float dx = left + 12 * s, dy = ry + rowH / 2 - 4 * s, dd = 8 * s;
                Color dc = st.State switch
                {
                    "run" => Color.White, "done" => Theme.Ready, "fail" => Theme.Bad, "dry" => Theme.Warn, _ => Theme.Dim
                };
                if (st.State == "wait")
                    using (var pen = new Pen(Theme.Dim, Math.Max(1, 1.2f * s))) g.DrawEllipse(pen, dx, dy, dd, dd);
                else
                    using (var b = new SolidBrush(dc)) g.FillEllipse(b, dx, dy, dd, dd);
                Color tc = st.State switch
                {
                    "run" => Theme.Text, "done" => Theme.Muted, "fail" => Theme.Bad, "dry" => Theme.Warn, _ => Theme.Dim
                };
                using (var f = Theme.Body(13 * s, st.State == "run"))
                    Theme.DrawText(g, st.Title, f, tc, left + 30 * s, ry, lw - 110 * s, rowH);
                string t = st.State switch
                {
                    "run" when st.T0 != null => FmtDur((DateTime.Now - st.T0.Value).TotalSeconds),
                    "skip" => "skipped",
                    "dry" => "would run",
                    "fail" => "failed",
                    _ => st.Secs > 0 ? FmtDur(st.Secs) : ""
                };
                using (var f = Theme.Body(12 * s))
                    Theme.DrawText(g, t, f, st.State == "run" ? Theme.Text : Theme.Dim, left + lw - 84 * s, ry, 74 * s, rowH, StringAlignment.Far);
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
            using (var f = Theme.Display(60 * s))
                Theme.DrawText(g, $"{(int)Math.Round(frac * 100)}%", f, Theme.Text, rx - 4 * s, 132 * s, 260 * s, 70 * s);

            float barY = 206 * s, barH = 10 * s;
            using (var p = Theme.Para(rx, barY, rw, barH, barH * 0.5f))
            using (var b = new SolidBrush(Color.FromArgb(26, 255, 255, 255)))
                g.FillPath(b, p);
            float fw = (float)(rw * frac);
            if (fw >= barH)
                using (var p = Theme.Para(rx, barY, fw, barH, barH * 0.5f))
                using (var b = new SolidBrush(failed ? Color.FromArgb(255, 150, 60, 60) : _succeeded ? Theme.Ready : Theme.Accent))
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
            using (var f = Theme.Body(13.5f * s, true))
                Theme.DrawText(g, curText, f, failed ? Theme.Bad : Theme.Text, rx, 222 * s, rw - 260 * s, 26 * s);
            using (var f = Theme.Body(12.5f * s))
                Theme.DrawText(g, eta, f, Theme.Muted, rx + rw - 260 * s, 222 * s, 260 * s, 26 * s, StringAlignment.Far);

            // What stopped it
            float logLabelY = 262 * s;
            if (failed)
            {
                float ey = 258 * s, eh = 104 * s;
                Theme.Card(g, rx, ey, rw, eh, 10 * s, s);
                using (var p = Theme.CutCorner(rx, ey, rw, eh, 16 * s))
                using (var pen = new Pen(Color.FromArgb(170, 255, 100, 100), Math.Max(1, s)))
                    g.DrawPath(pen, p);
                using (var b = new SolidBrush(Theme.Bad)) g.FillRectangle(b, rx + 1, ey + 1, 3 * s, eh - 2);
                using (var f = Theme.Display(17 * s))
                    Theme.DrawText(g, "WHAT STOPPED IT", f, Theme.Bad, rx + 20 * s, ey + 10 * s, rw - 40 * s, 22 * s);
                using (var f = Theme.Body(13 * s, true))
                    Theme.DrawWrapped(g, _errMsg, f, Theme.Text, rx + 20 * s, ey + 36 * s, rw - 40 * s, 34 * s);
                using (var f = Theme.Body(12.5f * s))
                    Theme.DrawWrapped(g, _errHint, f, Theme.Muted, rx + 20 * s, ey + 70 * s, rw - 40 * s, 30 * s);
                logLabelY = 374 * s;
            }

            // Log
            SectionLabel(g, "LOG", rx, logLabelY);
            float ly = logLabelY + 24 * s, lh = y1 - ly;
            Theme.Card(g, rx, ly, rw, lh, 10 * s, s);
            Hit("log", rx, ly, rw, lh);
            float lineH = 15 * s;
            int rows = Math.Max(1, (int)((lh - 16 * s) / lineH));
            _logScroll = Math.Clamp(_logScroll, 0, Math.Max(0, _log.Count - rows));
            int start = Math.Max(0, _log.Count - rows - _logScroll);
            using (var f = Theme.Mono(11.5f * s))
                for (int i = 0; i < rows && start + i < _log.Count; i++)
                {
                    string line = _log[start + i];
                    Color c = line.StartsWith("ERROR") || line.Contains("FAILED") ? Theme.Bad
                        : line.StartsWith("  hint:") ? Theme.Warn : Theme.Dim;
                    Theme.DrawText(g, line, f, c, rx + 14 * s, ly + 8 * s + i * lineH, rw - 28 * s, lineH);
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
            Color col = text == _status ? (_statusColor == Theme.Muted ? Theme.Muted : Blend(_statusColor)) : Theme.Text;
            Color dot = text != _status ? Theme.Muted : _running ? Theme.Accent : _statusColor == Theme.Muted ? Theme.Ready : _statusColor;
            StatusMark(g, dot, left, bottom + 14 * s);
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawText(g, text, f, col, left + 15 * s, bottom, W - 2 * left - 340 * s, 28 * s);
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawText(g, "First-time setup", f, Theme.Muted, W - left - 260 * s, bottom - 14 * s, 260 * s, 22 * s, StringAlignment.Far);
            using (var f = Theme.Body(12 * s))
                Theme.DrawText(g, "OpenSmash-Ultimate \u2014 native PC port", f, Theme.Dim, W - left - 320 * s, bottom + 8 * s, 320 * s, 20 * s, StringAlignment.Far);
        }

        void PaintModal(Graphics g, int W, int H)
        {
            var m = _modal!;
            float s = _s;
            _hits.Clear();
            using (var b = new SolidBrush(Color.FromArgb(180, 0, 0, 0))) g.FillRectangle(b, 0, 0, W, H);

            float mw = (m.Mid != "" ? 720 : 540) * s, pad = 26 * s;
            float bodyH;
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
                Theme.Label(g, m.Title.ToUpperInvariant(), f, Theme.Text, mx + pad, my + pad + 4 * s, 26 * s, 0.28f * s);
            using (var f = Theme.Body(13.5f * s))
                Theme.DrawWrapped(g, m.Body, f, Theme.Muted, mx + pad, my + pad + 46 * s, mw - 2 * pad, bodyH);

            float gap = 12 * s, bh = 44 * s, by = my + mh - pad - bh;
            int nBtn = 1 + (m.Mid != "" ? 1 : 0) + (m.No != "" ? 1 : 0);
            float bw = Math.Min(190 * s, (mw - 2 * pad - gap * (nBtn - 1)) / nBtn);
            float yesX = mx + mw - pad - bw;
            Theme.Button(g, m.Yes, yesX, by, bw, bh, s, primary: true, hot: _hover == "modal:yes");
            Hit("modal:yes", yesX, by, bw, bh);
            float left = yesX;
            if (m.Mid != "")
            {
                left -= gap + bw;
                Theme.Button(g, m.Mid, left, by, bw, bh, s, primary: false, hot: _hover == "modal:mid");
                Hit("modal:mid", left, by, bw, bh);
            }
            if (m.No != "")
            {
                left -= gap + bw;
                Theme.Button(g, m.No, left, by, bw, bh, s, primary: false, hot: _hover == "modal:no");
                Hit("modal:no", left, by, bw, bh);
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

        // ---------------------------------------------------------------------------
        // Input
        void OnMouseMove(object? sender, MouseEventArgs e)
        {
            string id = HitAt(e.Location);
            if (id == _hover) return;
            _hover = id;
            bool clickable = id != "" && !id.StartsWith("tip:") && !id.StartsWith("chk:") && !id.StartsWith("step:") && id != "log";
            Cursor = clickable ? Cursors.Hand : Cursors.Default;
            Invalidate();
        }

        void OnMouseDown(object? sender, MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            string id = HitAt(e.Location);
            bool clickable = id != "" && !id.StartsWith("tip:") && !id.StartsWith("chk:") && !id.StartsWith("step:") && id != "log";
            if (clickable) { _pressed = id; return; }
            if (_modal != null) return;
            ReleaseCapture();
            SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);     // drag the borderless window
        }

        void OnMouseUp(object? sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && _pressed != "" && HitAt(e.Location) == _pressed) Activate(_pressed);
            _pressed = "";
        }

        void OnMouseWheel(object? sender, MouseEventArgs e)
        {
            if (_view != "build") return;
            _logScroll += e.Delta / 120 * 3;
            Invalidate();
        }

        void OnKeyDown(object? sender, KeyEventArgs e)
        {
            if (_modal != null)
            {
                if (e.KeyCode == Keys.Escape) Activate(_modal.No != "" ? "modal:no" : "modal:yes");
                else if (e.KeyCode == Keys.Enter) Activate("modal:yes");
                return;
            }
            if (e.KeyCode == Keys.Escape)
            {
                if (_view == "build" && !_running && _jobKind != "bootstrap") { _view = "setup"; Invalidate(); }
                else Close();
            }
        }

        void Activate(string id)
        {
            if (_modal != null && id != "modal:yes" && id != "modal:mid" && id != "modal:no") return;
            switch (id)
            {
                case "close": Close(); break;
                case "modal:yes": { var m = _modal; _modal = null; m?.OnYes?.Invoke(); break; }
                case "modal:mid": { var m = _modal; _modal = null; m?.OnMid?.Invoke(); break; }
                case "modal:no": { var m = _modal; _modal = null; m?.OnNo?.Invoke(); break; }

                case "pick:base:file": BrowseFile("base", "Base game (.xci, .nsp, .nsz, .nca)", "Switch game files|*.xci;*.xcz;*.nsp;*.nsz;*.nca|All files|*.*"); break;
                case "pick:base:folder": BrowseFolder("base", "Folder with exefs\\ and romfs\\ (a dump you already made)"); break;
                case "pick:update": BrowseFile("update", "Update 13.0.4 or 13.0.5 (.nsp, .nsz)", "Switch update|*.nsp;*.nsz|All files|*.*"); break;
                case "pick:prodkeys": BrowseFile("prodkeys", "prod.keys", "Keys|*.keys|All files|*.*"); break;
                case "pick:dlc": AddDlc(); break;
                case "clear:dlc": _dlc.Clear(); UpdateFieldStatus(); break;
                case "pick:buildloc": BrowseLocation(); break;
                case "default:buildloc": SetValue("buildloc", _defaultBuild); break;

                case "opt:shortcuts": _optShortcuts = !_optShortcuts; break;
                case "opt:firewall": _optFirewall = !_optFirewall; break;
                case "opt:shaders": _optShaders = !_optShaders; break;
                case "pick:game": PickFolder("game"); break;
                case "pick:keys": PickFolder("keys"); break;
                case "pick:firmware": PickFolder("firmware"); break;
                case "clear:firmware": _fwPath = ""; CheckLocal(); _ = CheckGameAsync(); break;
                case "act:check": CheckLocal(); _ = CheckGameAsync(); break;
                case "opt:updates": _optUpdates = !_optUpdates; break;
                case "opt:license": _optLicense = !_optLicense; break;
                case "opt:runtime":
                    _optRuntime = !_optRuntime;
                    if (!_running) StartPrecheck();      // the C++ Build Tools are checked only without the runtime
                    break;
                case "opt:fast": _optFast = !_optFast; break;
                case "opt:dry": _optDry = !_optDry; break;
                case "jobs:-": _jobs = Math.Max(1, _jobs - 1); break;
                case "jobs:+": _jobs = Math.Min(64, _jobs + 1); break;

                case "act:install": StartInstall(_optDry); break;
                case "act:dry": StartInstall(true); break;
                case "act:update": StartUpdate(); break;
                case "act:python": StartBootstrap(); break;
                case "act:logs": OpenLogs(); break;
                case "act:stop": StopJob(); break;
                case "act:resume": Resume(); break;
                case "act:back": _view = "setup"; UpdateFieldStatus(); break;
                case "act:msvc": InstallBuildTools(); break;
                case "act:continue": Close(); break;
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

        // ---------------------------------------------------------------------------
        // One child process with redirected output. In capture mode the whole output is
        // kept for parsing at the end; otherwise lines are queued for the timer.
        sealed class Proc
        {
            public readonly ConcurrentQueue<string> Lines = new();
            readonly bool _capture;
            readonly StringBuilder _out = new(), _err = new();
            Process? _p;
            public volatile bool Done;
            public int ExitCode = -1;
            public string StartError = "";

            public Proc(bool capture) { _capture = capture; }

            public string Output { get { lock (_out) return _out.ToString(); } }
            public string ErrorText { get { lock (_err) return _err.ToString(); } }

            public bool Start(string exe, IEnumerable<string> args, string cwd)
            {
                try
                {
                    var psi = new ProcessStartInfo(exe)
                    {
                        WorkingDirectory = cwd,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        RedirectStandardInput = true,
                        StandardOutputEncoding = Encoding.UTF8,
                        StandardErrorEncoding = Encoding.UTF8,
                    };
                    foreach (var a in args) psi.ArgumentList.Add(a);
                    psi.Environment["PYTHONIOENCODING"] = "utf-8";
                    psi.Environment["PYTHONUTF8"] = "1";
                    psi.Environment["PYTHONUNBUFFERED"] = "1";
                    var p = new Process { StartInfo = psi };
                    p.OutputDataReceived += (s, e) => Add(e.Data, _out);
                    p.ErrorDataReceived += (s, e) => Add(e.Data, _err);
                    p.Start();
                    _p = p;
                    p.BeginOutputReadLine();
                    p.BeginErrorReadLine();
                    new Thread(() =>
                    {
                        try { p.WaitForExit(); ExitCode = p.ExitCode; } catch { }
                        Done = true;
                    }) { IsBackground = true }.Start();
                    return true;
                }
                catch (Exception ex)
                {
                    StartError = ex.Message;
                    Done = true;
                    return false;
                }
            }

            void Add(string? line, StringBuilder sink)
            {
                if (line == null) return;
                if (_capture) { lock (sink) sink.AppendLine(line); }
                else Lines.Enqueue(line);
            }

            public void WriteLine(string text)
            {
                try
                {
                    var p = _p;
                    if (p == null || p.HasExited) return;
                    p.StandardInput.WriteLine(text);
                    p.StandardInput.Flush();
                }
                catch { }
            }

            // Stops only the process tree this object started (never by name).
            public void Kill()
            {
                if (Done || _p == null) return;
                try { _p.Kill(entireProcessTree: true); } catch { }
            }
        }
    }
}
