// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.5) Setup error reports: when the first install, an update or the game's build stops with an error, when the launcher
// itself crashes, when the game was not rebuilt after an update (StaleGame.cs) or could not start (its game library did not
// load, or it ended before writing a log line), the launcher sends a report on its own, the way SSBU Arena's launcher does
// (its LogReport.cs AutoSend): the accounts service's edge function logs-send, product 'opensmash', with or without an
// account (the server limits a sender by a hash of its address and the day). Until now only a signed-in player's game
// crashes reached us (CrashReport.cs): no failed install or update ever did.
//
// A report: kind (install_error, update_error, build_error, launcher_crash, stale_game, game_start_error), a short error code
// (the first 8 hex digits of a SHA-256 of the kind and the error's first line made stable: the same error has the same
// code on every PC, the player can quote it on Discord or GitHub), that first line, where it stopped, the launcher's version,
// the sources' commit and the build's, the OS and the GPU when known, and the end of the relevant logs (installer, update,
// build, the mods' manager, the plugin probe; each bounded, 512 KB in all) cleaned as the crash reports are
// (CrashReport.Clean: user and PC names, folders, e-mail and internet addresses, tickets) and with anything that looks like
// a token or key masked (Mask).
//
// Limits here (the server has its own): at most MaxPerRun reports a launcher run, the same code once in 24 h on this PC
// (a small file in the user's local application data), one at a time.
// Switches: OPENSMASH_SETUP_REPORTS=0 (or off): none, and the launcher's own crash handlers are not installed - as before;
// the settings' "Send setup error reports" (config/setup_reports.txt "off"); the crash reports' own setting off
// (config/crash_reports.txt "off") stops them too. OPENSMASH_SETUP_REPORT_TEST=1 (or a file path): the request is written
// to build/setup_report_test.json (or that path) and not sent.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    /// <summary>what stopped: what a report is made of</summary>
    public sealed class SetupFailure
    {
        /// <summary>SetupReport.Kinds: install_error, update_error, build_error, launcher_crash, stale_game, game_start_error</summary>
        public string Kind;
        /// <summary>where it stopped, for the developers ("install_opensmash.ps1, step Build the game", "update: launcher replaced")</summary>
        public string Where;
        /// <summary>the error as the player saw it (its first line is the report's error line and makes its code)</summary>
        public string Error;
        public int? ExitCode;
        /// <summary>the install's folder, null before there is one (the first download)</summary>
        public string Root;
        /// <summary>what the step printed, when the launcher kept it in memory (the update's output): its end is sent</summary>
        public string Output;
        /// <summary>log files besides the usual ones (SetupReport.LogFiles)</summary>
        public IEnumerable<string> Logs;
        /// <summary>an exception's whole text (its stack), for launcher_crash</summary>
        public string Detail;
    }

    public sealed class SetupReportResult
    {
        public string Code, ServerCode, Error;
        /// <summary>sent: the server took it; duplicate: this code was sent already within 24 h (here or as the server says);
        /// off: reports are off; limited: this run's MaxPerRun are used up; test: written to a file, not sent</summary>
        public bool Sent, Duplicate, Off, Limited, Test;

        /// <summary>the line the player sees, with the code to quote</summary>
        public string Message =>
            Sent ? "Error code " + Code + " — this was sent to the developers. You can quote it on Discord or GitHub."
            : Duplicate ? "Error code " + Code + " — the developers already have this error (sent in the last 24 hours)."
            : Test ? "Error code " + Code + " — test mode: the report was written, not sent."
            : Off ? "Error code " + Code + " — setup error reports are off: quote this code on Discord or GitHub."
            : "Error code " + Code + " — the report could not be sent" + (string.IsNullOrEmpty(Error) ? "" : " (" + Error + ")") + ": quote this code on Discord or GitHub.";
    }

    public static class SetupReport
    {
        public const string Variable = "OPENSMASH_SETUP_REPORTS", TestVariable = "OPENSMASH_SETUP_REPORT_TEST";
        public const string SettingFile = "setup_reports.txt", CrashSettingFile = "crash_reports.txt";
        public const string Product = "opensmash";
        public const int MaxPerRun = 3;
        public static readonly TimeSpan Window = TimeSpan.FromHours(24);
        public const int MaxText = 512 * 1024;        // bytes of the whole text (logs-send takes 1.2 MB)
        public const int MaxOutput = 96 * 1024;       // of an output kept in memory
        public static readonly string[] Kinds = { "install_error", "update_error", "build_error", "launcher_crash", "stale_game", "game_start_error" };

        // ------------------------------------------------------------------ switches
        /// <summary>OPENSMASH_SETUP_REPORTS: on unless "0" or "off"</summary>
        public static bool SwitchOn(string value)
        {
            string v = (value ?? "").Trim();
            return !(v == "0" || v.Equals("off", StringComparison.OrdinalIgnoreCase) || v.Equals("false", StringComparison.OrdinalIgnoreCase));
        }

        public static bool SwitchOn() => SwitchOn(Environment.GetEnvironmentVariable(Variable));

        static bool FileSaysOff(string path)
        {
            try { return File.Exists(path) && File.ReadAllText(path).Trim().StartsWith("off", StringComparison.OrdinalIgnoreCase); }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }

        /// <summary>the player's setting "Send setup error reports": on unless config/setup_reports.txt starts with "off"</summary>
        public static bool SettingOn(string root) => string.IsNullOrEmpty(root) || !FileSaysOff(Path.Combine(root, "config", SettingFile));

        public static void SetSetting(string root, bool on)
        {
            Directory.CreateDirectory(Path.Combine(root, "config"));
            File.WriteAllText(Path.Combine(root, "config", SettingFile), on ? "on\n" : "off\n");
        }

        /// <summary>reports go out: the switch, this setting, and the crash reports' setting (config/crash_reports.txt) all on</summary>
        public static bool Enabled(string root) =>
            SwitchOn() && SettingOn(root) && (string.IsNullOrEmpty(root) || !FileSaysOff(Path.Combine(root, "config", CrashSettingFile)));

        // ------------------------------------------------------------------ kind and code
        static readonly Regex BuildStep = new Regex(@"\bbuild(ing)?\s+the\s+game\b|\bbuild_game\b|\bthe\s+(game\s+)?(re)?build\s+stopped\b|\bcompil(e|ing|er\s+error)|\blink(ing|er)?\s+(error|failed)|\bLNK\d{4}\b|\bskyline\b|\barcropolis\b|\bplugin(s)?\b.*\b(build|compile|failed)|\brecomp(\.py)?\b.*\b(fail|error)", RegexOptions.IgnoreCase);

        /// <summary>the kind of a flow's failure. flow: install | bootstrap | first_run | update | rebuild | build | launcher |
        /// stale | game_start; step: the installer's step that failed (its id or title), if known. An install or update whose
        /// failing step is the game's build (or whose error says so) is a build_error.</summary>
        public static string KindFor(string flow, string step = null, string error = null)
        {
            switch ((flow ?? "").Trim().ToLowerInvariant())
            {
                case "launcher": return "launcher_crash";
                case "stale": return "stale_game";
                case "game_start": return "game_start_error";
                case "rebuild": case "build": return "build_error";
            }
            bool building = (!string.IsNullOrEmpty(step) && (Regex.IsMatch(step, @"^(build|game|compile|recomp|plugins?)$", RegexOptions.IgnoreCase) || BuildStep.IsMatch(step)))
                            || (!string.IsNullOrEmpty(error) && BuildStep.IsMatch(FirstLine(error)));
            if (building) return "build_error";
            return (flow ?? "").Trim().Equals("update", StringComparison.OrdinalIgnoreCase) ? "update_error" : "install_error";
        }

        /// <summary>the first line with something on it</summary>
        public static string FirstLine(string s) =>
            (s ?? "").Replace("\r", "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0) ?? "";

        /// <summary>the first line as the code is made of it: cleaned, lower case, times, long numbers and hex addresses
        /// replaced, spaces collapsed - the same error on two PCs (other folders, another time) has the same form</summary>
        public static string Stable(string firstLine, string root = null)
        {
            string t = Scrub(FirstLine(firstLine), root).ToLowerInvariant();
            // (this PC's user folder and another's read the same)
            t = Regex.Replace(t, @"<home>|\b[a-z]:[\\/]+(users|documents and settings)[\\/]+<user>|/(home|users)/<user>", "<home>");
            t = Regex.Replace(t, @"\b\d{4}-\d{2}-\d{2}([ t]\d{2}:\d{2}(:\d{2})?)?\b", "<time>");
            t = Regex.Replace(t, @"\b\d{1,2}:\d{2}(:\d{2})?\b", "<time>");
            t = Regex.Replace(t, @"\b0x[0-9a-f]{9,}\b", "<addr>");
            t = Regex.Replace(t, @"\b[0-9a-f]{12,}\b", "<hex>");
            t = Regex.Replace(t, @"\b\d{6,}\b", "<n>");
            t = Regex.Replace(t, @"\d+(\.\d+)?\s*(ms|s|seconds?|minutes?|mb|gb|kb|%)\b", "<n>$2");
            t = Regex.Replace(t, @"\s+", " ").Trim();
            return t.Length > 200 ? t.Substring(0, 200) : t;
        }

        /// <summary>the error code: 8 hex digits (upper case) of a SHA-256 of the kind and the first line's stable form</summary>
        public static string Code(string kind, string error, string root = null)
        {
            byte[] h = SHA256.HashData(Encoding.UTF8.GetBytes((kind ?? "") + "|" + Stable(error, root)));
            return Convert.ToHexString(h, 0, 4);
        }

        /// <summary>an exception as one line: its type, its HRESULT when it is a Windows error (a file in use is 0x80070020,
        /// access denied 0x80070005), its message</summary>
        public static string Describe(Exception ex)
        {
            if (ex == null) return "";
            var b = ex is AggregateException a && a.InnerExceptions.Count == 1 ? a.InnerExceptions[0] : ex;
            string hr = (b is IOException || b is UnauthorizedAccessException || b is System.ComponentModel.Win32Exception || b is COMException) && b.HResult != 0
                ? " 0x" + b.HResult.ToString("X8", CultureInfo.InvariantCulture) : "";
            return b.GetType().Name + hr + ": " + FirstLine(b.Message);
        }

        // ------------------------------------------------------------------ cleaning
        /// <summary>CrashReport.Clean (the install's folder, the user's folder, user and PC names, tickets, signatures, internet
        /// and e-mail addresses), then anything that looks like a secret: JWTs, Supabase keys, "Bearer ...", key = value pairs
        /// whose name says key, token, secret, password or ticket, such values in an address</summary>
        public static string Scrub(string t, string root = null)
        {
            if (string.IsNullOrEmpty(t)) return t ?? "";
            t = t.Replace("﻿", "").Replace("\0", "");
            try { t = root == null ? CrashReport.Clean(t) : CrashReport.Clean(t, root); } catch (RegexMatchTimeoutException) { }
            t = Regex.Replace(t, @"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}", "<token>");
            t = Regex.Replace(t, @"\bsb_(publishable|secret)_[A-Za-z0-9_-]{8,}", "<key>");
            t = Regex.Replace(t, @"\bgh[pousr]_[A-Za-z0-9]{20,}", "<key>");
            t = Regex.Replace(t, @"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{8,}", "Bearer <token>");
            t = Regex.Replace(t, @"(?im)\b([\w.-]*(key|token|secret|password|passwd|ticket|apikey|refresh)[\w.-]*)(\s*[=:]\s*)(""?)[^\s""]{6,}\4", "$1$3<hidden>");
            t = Regex.Replace(t, @"(?i)([?&](token|key|ticket|state|code|refresh_token|access_token)=)[^&\s""]+", "$1<hidden>");
            // a Windows user folder the other rules did not know (another account's, a path written on another PC)
            t = Regex.Replace(t, @"(?i)\b([A-Z]:[\\/]+(Users|Documents and Settings)[\\/]+)[^\\/\r\n""'<>|:*?]+", "$1<user>");
            t = Regex.Replace(t, @"(/(home|Users)/)[^/\s""']+", "$1<user>");
            return t;
        }

        // ------------------------------------------------------------------ the logs
        /// <summary>the usual logs of the setup, the update and the build, with how much of each is sent (their end, and a
        /// little of their start): those that exist</summary>
        public static List<(string Name, string Path, int Max)> LogFiles(string root, IEnumerable<string> extra = null)
        {
            var list = new List<(string, string, int)>();
            void Add(string path, string name, int max)
            {
                try
                {
                    if (string.IsNullOrEmpty(path)) return;
                    var fi = new FileInfo(path);
                    if (!fi.Exists || fi.Length == 0 || list.Any(x => string.Equals(x.Item2, fi.FullName, StringComparison.OrdinalIgnoreCase))) return;
                    list.Add((name, fi.FullName, max));
                }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException || e is NotSupportedException) { }
            }
            if (extra != null) foreach (var x in extra) Add(x, Path.GetFileName(x ?? ""), 96 * 1024);
            if (!string.IsNullOrEmpty(root))
            {
                foreach (var (rel, max) in new (string, int)[]
                {
                    ("build/launcher_crash.log", 48 * 1024), ("build/logs/installer_window.log", 96 * 1024), ("install.log", 96 * 1024),
                    ("build/launcher_update.log", 32 * 1024), ("build/logs/source_update.log", 48 * 1024), ("build/logs/update_game_build.log", 96 * 1024),
                    ("build/logs/python_setup.log", 24 * 1024), ("build/logs/shader_window.log", 16 * 1024),
                    ("dump/mods/manager.log", 48 * 1024), ("dump/skyline/probe.log", 16 * 1024), ("build/build_commit.txt", 4 * 1024),
                })
                    Add(Path.Combine(root, rel.Replace('/', Path.DirectorySeparatorChar)), rel, max);
            }
            return list;
        }

        /// <summary>the GPU, from the game's log when there is one (as CrashReport does), else null</summary>
        public static string Gpu(string root)
        {
            try
            {
                if (string.IsNullOrEmpty(root)) return null;
                string log = Path.Combine(root, "build", "play.log");
                if (!File.Exists(log)) return null;
                string head = CrashReport.HeadAndTail(log, 64 * 1024, 48 * 1024);
                var m = Regex.Match(head, @"(?:Vulkan|D3D11|Metal|device)[^\r\n]{0,20}:\s*([^\r\n(]{4,80})");
                return m.Success ? m.Groups[1].Value.Trim() : null;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { return null; }
        }

        public static string Os() =>
            RuntimeInformation.OSDescription.Trim() + " " + RuntimeInformation.OSArchitecture.ToString().ToLowerInvariant();

        static string BuildLine(string root)
        {
            if (string.IsNullOrEmpty(root)) return "no install yet";
            var parts = new List<string>();
            try
            {
                string exe = Path.Combine(root, "build", OperatingSystem.IsWindows() ? "ssbu.exe" : "ssbu");
                parts.Add(File.Exists(exe) ? "game program of " + File.GetLastWriteTimeUtc(exe).ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + " UTC" : "no game program");
                string rec = StaleGame.RecordedCommit(root);
                parts.Add(rec != null ? "built from " + rec : "build commit not recorded");
                if (File.Exists(Updater.PendingMarker(root))) parts.Add("an update's rebuild is pending");
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
            return string.Join("; ", parts);
        }

        /// <summary>the report's text: a header (code, error, where, versions, OS, GPU), then the output and each log's end</summary>
        public static string BuildText(SetupFailure f, string code, DateTime utcNow, string gpu)
        {
            var sb = new StringBuilder();
            string commit = string.IsNullOrEmpty(f.Root) ? null : Updater.InstalledCommit(f.Root);
            sb.Append("OpenSmash setup report (").Append(f.Kind).Append(")\n");
            sb.Append("code:      ").Append(code).Append('\n');
            sb.Append("error:     ").Append(FirstLine(f.Error)).Append('\n');
            if (!string.IsNullOrEmpty(f.Where)) sb.Append("where:     ").Append(f.Where).Append('\n');
            if (f.ExitCode is int x) sb.Append("exit code: ").Append(x.ToString(CultureInfo.InvariantCulture)).Append(" (0x").Append(x.ToString("X8", CultureInfo.InvariantCulture)).Append(")\n");
            sb.Append("sent:      ").Append(utcNow.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)).Append(" UTC\n");
            sb.Append("launcher:  ").Append(Updater.Current.ToString()).Append(OperatingSystem.IsWindows() ? "" : " (shared launcher)").Append('\n');
            sb.Append("sources:   ").Append(commit ?? "not known").Append(string.IsNullOrEmpty(f.Root) ? "" : ", game " + (VersionInfo.Game(f.Root) is string g && g.Length > 0 ? g : "?")).Append('\n');
            sb.Append("build:     ").Append(BuildLine(f.Root)).Append('\n');
            sb.Append("os:        ").Append(Os()).Append(", ").Append(Environment.ProcessorCount).Append(" threads, culture ").Append(CultureInfo.CurrentCulture.Name).Append('\n');
            sb.Append("gpu:       ").Append(gpu ?? "not known").Append('\n');
            var files = LogFiles(f.Root, f.Logs);
            sb.Append("files:     ").Append(files.Count == 0 ? "none found" : string.Join(", ", files.Select(l => l.Name))).Append('\n');
            string full = (f.Error ?? "").Trim();
            if (full.Contains('\n')) sb.Append("\n==================== the error as shown\n").Append(Cut(full.Replace("\r\n", "\n"), 8 * 1024)).Append('\n');
            if (!string.IsNullOrEmpty(f.Detail)) sb.Append("\n==================== exception\n").Append(Cut(f.Detail.Replace("\r\n", "\n"), 16 * 1024)).Append('\n');
            if (!string.IsNullOrEmpty(f.Output)) sb.Append("\n==================== output (its end)\n").Append(TailText(f.Output.Replace("\r\n", "\n"), MaxOutput)).Append('\n');
            foreach (var (name, path, max) in files)
            {
                string body;
                try { body = CrashReport.HeadAndTail(path, max, Math.Min(8 * 1024, max / 8)); }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { body = "(not readable: " + e.Message + ")"; }
                long size = 0;
                try { size = new FileInfo(path).Length; } catch (IOException) { }
                sb.Append("\n==================== ").Append(name).Append("  (").Append(size.ToString("N0", CultureInfo.InvariantCulture)).Append(" bytes)\n");
                sb.Append(body.Replace("\r\n", "\n"));
                if (!body.EndsWith("\n", StringComparison.Ordinal)) sb.Append('\n');
            }
            return Bound(Scrub(sb.ToString(), f.Root), MaxText);
        }

        static string Cut(string t, int max) => t.Length <= max ? t : t.Substring(0, max) + "\n[... cut ...]";

        static string TailText(string t, int max)
        {
            if (t.Length <= max) return t;
            int nl = t.IndexOf('\n', t.Length - max);
            return "[... " + (t.Length - max).ToString(CultureInfo.InvariantCulture) + " characters before ...]\n" + t.Substring(nl >= 0 ? nl + 1 : t.Length - max);
        }

        /// <summary>the text within maxBytes of UTF-8: its first quarter and its end, a line between them</summary>
        public static string Bound(string t, int maxBytes)
        {
            if (Encoding.UTF8.GetByteCount(t) <= maxBytes) return t;
            int head = maxBytes / 4, tail = maxBytes - head - 64;
            string a = t.Substring(0, Math.Min(t.Length, head));
            while (Encoding.UTF8.GetByteCount(a) > head) a = a.Substring(0, a.Length * 9 / 10);
            string b = t.Substring(Math.Max(0, t.Length - tail));
            while (Encoding.UTF8.GetByteCount(b) > tail) b = b.Substring(b.Length / 10);
            return a + "\n\n[... cut ...]\n\n" + b;
        }

        // ------------------------------------------------------------------ the request
        /// <summary>logs-send's request (its schema.ts in the accounts service's repository): product, kind, text, exception
        /// (the code and the first line), version, commit, windows (the OS: the column's name), gpu</summary>
        public static Dictionary<string, object> Payload(SetupFailure f, string code, string text, string gpu)
        {
            string first = Scrub(FirstLine(f.Error), f.Root);
            string exception = "[" + code + "] " + (string.IsNullOrEmpty(f.Where) ? "" : Scrub(f.Where, f.Root) + ": ") + first;
            if (exception.Length > 400) exception = exception.Substring(0, 400);
            string os = Os();
            var d = new Dictionary<string, object>
            {
                ["product"] = Product, ["kind"] = f.Kind, ["text"] = text, ["exception"] = exception,
                ["version"] = Updater.Current.ToString(),
                ["commit"] = string.IsNullOrEmpty(f.Root) ? null : Updater.InstalledCommit(f.Root),
                ["windows"] = os.Length > 100 ? os.Substring(0, 100) : os,
            };
            if (!string.IsNullOrEmpty(gpu)) d["gpu"] = gpu.Length > 200 ? gpu.Substring(0, 200) : gpu;
            return d;
        }

        /// <summary>the report's code, text and request for a failure (nothing sent)</summary>
        public static (string Code, string Json) Build(SetupFailure f, DateTime? utcNow = null)
        {
            if (f == null) throw new ArgumentNullException(nameof(f));
            if (!Kinds.Contains(f.Kind)) throw new ArgumentException("not a setup report kind: " + f.Kind);
            string code = Code(f.Kind, f.Error, f.Root);
            string gpu = Gpu(f.Root);
            string text = BuildText(f, code, utcNow ?? DateTime.UtcNow, gpu);
            return (code, JsonSerializer.Serialize(Payload(f, code, text, gpu)));
        }

        // ------------------------------------------------------------------ limits
        /// <summary>MaxPerRun a launcher run, the same code once in Window (kept in a file), as one place to ask</summary>
        public sealed class Limiter
        {
            readonly string _file;
            readonly int _perRun;
            readonly TimeSpan _window;
            readonly object _gate = new object();
            int _taken;

            public Limiter(string file, int perRun = MaxPerRun, TimeSpan? window = null)
            {
                _file = file; _perRun = perRun; _window = window ?? Window;
            }

            public int Taken { get { lock (_gate) return _taken; } }

            /// <summary>the codes sent within the window (older ones dropped)</summary>
            public Dictionary<string, DateTime> Sent(DateTime utcNow)
            {
                var d = new Dictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);
                try
                {
                    if (_file == null || !File.Exists(_file)) return d;
                    foreach (string line in File.ReadAllLines(_file))
                    {
                        var p = line.Trim().Split(' ');
                        if (p.Length == 2 && Regex.IsMatch(p[0], "^[0-9A-Fa-f]{8}$")
                            && DateTime.TryParse(p[1], CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out var at)
                            && utcNow - at < _window && at <= utcNow.AddMinutes(5))
                            d[p[0].ToUpperInvariant()] = at;
                    }
                }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
                return d;
            }

            /// <summary>"" when code may go now (it is counted), else why not: "duplicate" (sent within the window) or "limit"
            /// (this run's are used up)</summary>
            public string Take(string code, DateTime utcNow)
            {
                lock (_gate)
                {
                    if (Sent(utcNow).ContainsKey(code)) return "duplicate";
                    if (_taken >= _perRun) return "limit";
                    _taken++;
                    return "";
                }
            }

            /// <summary>code sent (or known to the server): not again within the window</summary>
            public void Remember(string code, DateTime utcNow)
            {
                lock (_gate)
                {
                    try
                    {
                        if (_file == null) return;
                        var d = Sent(utcNow);
                        d[code.ToUpperInvariant()] = utcNow;
                        Directory.CreateDirectory(Path.GetDirectoryName(_file));
                        File.WriteAllLines(_file, d.Select(kv => kv.Key + " " + kv.Value.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture)));
                    }
                    catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
                }
            }
        }

        /// <summary>where the codes sent are kept: the user's local application data (no install needed)</summary>
        public static string SentFile()
        {
            string dir = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrEmpty(dir)) dir = Path.GetTempPath();
            return Path.Combine(dir, "OpenSmash", "setup_reports_sent.txt");
        }

        static Limiter s_limiter;
        public static Limiter Default => s_limiter ??= new Limiter(SentFile());
        /// <summary>another limiter for what follows (tests: more than a run's 3, no file)</summary>
        public static void UseLimiter(Limiter limiter) => s_limiter = limiter;
        static readonly SemaphoreSlim s_one = new SemaphoreSlim(1, 1);
        static readonly Lazy<HttpClient> s_http = new Lazy<HttpClient>(() =>
        {
            var c = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
            c.DefaultRequestHeaders.UserAgent.ParseAdd("opensmash-launcher/" + Updater.Current);
            return c;
        });

        // ------------------------------------------------------------------ sending
        /// <summary>sends the report of f (anonymous: no session needed), within the limits; every outcome goes to log. Never
        /// throws. http/limiter/service: the tests'.</summary>
        public static async Task<SetupReportResult> SendAsync(SetupFailure f, Action<string> log = null, HttpClient http = null,
                                                              Limiter limiter = null, AccountService service = null, DateTime? utcNow = null)
        {
            var now = utcNow ?? DateTime.UtcNow;
            var r = new SetupReportResult();
            void Log(string s) { try { log?.Invoke(s); } catch { } }
            try
            {
                r.Code = Code(f.Kind, f.Error, f.Root);
                if (!Enabled(f.Root)) { r.Off = true; Log("setup report " + r.Code + " (" + f.Kind + "): off, not sent"); return r; }
                limiter ??= Default;
                string why = limiter.Take(r.Code, now);
                if (why == "duplicate") { r.Duplicate = true; Log("setup report " + r.Code + ": sent already within 24 h"); return r; }
                if (why == "limit") { r.Limited = true; r.Error = "this run sent " + MaxPerRun + " reports already"; Log("setup report " + r.Code + ": " + r.Error); return r; }
                var (code, json) = await Task.Run(() => Build(f, now));
                string test = Environment.GetEnvironmentVariable(TestVariable);
                if (!string.IsNullOrWhiteSpace(test) && test.Trim() != "0")
                {
                    string file = test.Trim() == "1"
                        ? Path.Combine(string.IsNullOrEmpty(f.Root) ? Path.GetTempPath() : Path.Combine(f.Root, "build"), "setup_report_test.json")
                        : test.Trim();
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file)));
                    File.WriteAllText(file, json);
                    r.Test = true;
                    Log("setup report " + code + " (test): written to " + file + ", not sent");
                    return r;
                }
                service ??= AccountService.Load(string.IsNullOrEmpty(f.Root) ? null : f.Root);
                if (!service.Configured) { r.Error = "no accounts service in this build"; Log("setup report " + code + ": " + r.Error); return r; }
                await s_one.WaitAsync();
                try
                {
                    using var rq = new HttpRequestMessage(HttpMethod.Post, service.Url + "/functions/v1/logs-send")
                    { Content = new StringContent(json, Encoding.UTF8, "application/json") };
                    rq.Headers.Add("apikey", service.Key);
                    using var rs = await (http ?? s_http.Value).SendAsync(rq);
                    string body = await rs.Content.ReadAsStringAsync();
                    int status = (int)rs.StatusCode;
                    string err = null;
                    try
                    {
                        using var doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
                        if (doc.RootElement.ValueKind == JsonValueKind.Object)
                        {
                            if (doc.RootElement.TryGetProperty("code", out var c) && c.ValueKind == JsonValueKind.String) r.ServerCode = c.GetString();
                            if (doc.RootElement.TryGetProperty("error", out var e) && e.ValueKind == JsonValueKind.String) err = e.GetString();
                        }
                    }
                    catch (JsonException) { }
                    if (status == 200) { r.Sent = true; limiter.Remember(code, now); Log("setup report " + code + " (" + f.Kind + ") sent: report " + (r.ServerCode ?? "?")); }
                    else if (status == 409) { r.Duplicate = true; limiter.Remember(code, now); Log("setup report " + code + ": the server has it already"); }
                    else
                    {
                        r.Error = status == 429 ? "too many reports, the server says" : "the server answered " + status + (err != null ? " " + err : "");
                        Log("setup report " + code + " not sent: " + r.Error);
                    }
                }
                finally { s_one.Release(); }
            }
            catch (Exception ex) when (ex is HttpRequestException || ex is TaskCanceledException || ex is IOException || ex is UnauthorizedAccessException
                                       || ex is InvalidOperationException || ex is ArgumentException || ex is JsonException)
            {
                r.Error = ex is TaskCanceledException ? "no answer in time" : FirstLine(ex.Message);
                Log("setup report " + r.Code + " not sent: " + r.Error);
            }
            return r;
        }
    }
}
