// SPDX-License-Identifier: GPL-3.0-or-later
// After a game: the session's performance summary the game wrote (reports\perf_session.json: runtime/debug_perf_session.c)
// is sent to the accounts service (public.perf_session_add, p_product = 'opensmash'), so that the project can see how
// the game runs on players' PCs: which hardware is slow, where frames are late, how heavy the rollback is.
// What is sent: the game's aggregates (frame time percentiles and histogram, stutter and shader wait counts, rollback
// totals, memory use) and the PC's kind (processor and graphics card model, driver, Windows version, RAM in GB, the
// renderer and resolution, the launcher's version). Never: a path, a name, an address, anything per frame.
// Only when signed in, and not when config\telemetry.txt says "off" (the launcher's setting). Sessions under a minute
// are not sent. Tests: OPENSMASH_PERF_TEST=1 writes build\perf_test_payload.json instead of sending.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public static class PerfReport
    {
        public const string FileName = "perf_session.json", SettingFile = "telemetry.txt";
        /// <summary>the fields of the game's summary that are sent, each a number (anything else in the file is dropped)</summary>
        static readonly string[] Numbers =
        {
            "session_s", "frames", "ft_p50", "ft_p95", "ft_p99", "ft_max", "late_pct", "stutters", "shader_compiles", "shader_hitch_ms",
            "rb_fights", "rb_rollbacks", "rb_tick_p50", "rb_tick_p99", "rb_over_pct", "rb_desyncs", "commit_peak_mb", "ram_avail_min_mb",
        };

        /// <summary>the player's setting: on unless config\telemetry.txt starts with "off"</summary>
        public static bool Enabled(string root)
        {
            try { string f = Path.Combine(root, "config", SettingFile); return !(File.Exists(f) && File.ReadAllText(f).Trim().StartsWith("off", StringComparison.OrdinalIgnoreCase)); }
            catch { return true; }
        }
        public static void SetEnabled(string root, bool on)
        {
            Directory.CreateDirectory(Path.Combine(root, "config"));
            File.WriteAllText(Path.Combine(root, "config", SettingFile), on ? "on\n" : "off\n");
        }

        /// <summary>what is sent for the summary text `json`, or null when it is not a summary worth sending (unreadable,
        /// under a minute, no frames). pc: the PC's kind (Pc(), or a test's); log: the end of play.log, for the graphics
        /// card and renderer lines (may be null).</summary>
        public static Dictionary<string, object> Payload(string json, IDictionary<string, object> pc, string log, string version, string commit)
        {
            JsonElement j;
            try { j = JsonDocument.Parse(json).RootElement; } catch { return null; }
            if (j.ValueKind != JsonValueKind.Object) return null;
            double Num(string k) => j.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out double d) && !double.IsNaN(d) && !double.IsInfinity(d) ? d : double.NaN;
            if (!(Num("session_s") >= 60) || !(Num("frames") >= 600)) return null;
            var p = new Dictionary<string, object> { ["version"] = version ?? "?", ["game_commit"] = commit };
            foreach (string k in Numbers) { double d = Num(k); if (!double.IsNaN(d)) p[k] = d; }
            if (j.TryGetProperty("hist", out var h) && h.ValueKind == JsonValueKind.Object)
            {
                var hist = new Dictionary<string, long>();
                foreach (var e in h.EnumerateObject().Take(16))
                    if (e.Name.Length <= 8 && e.Value.ValueKind == JsonValueKind.Number && e.Value.TryGetInt64(out long n)) hist[e.Name] = n;
                p["hist"] = hist;
            }
            p["crashed"] = !(j.TryGetProperty("clean_exit", out var c) && c.ValueKind == JsonValueKind.True);
            if (pc != null) foreach (var kv in pc) if (kv.Value != null) p[kv.Key] = kv.Value;
            if (log != null)
            {   // (the game's own lines: its graphics card, driver and renderer; never a path or a name)
                var m = Regex.Match(log, @"\[(vulkan|gfx|d3d11)\][^\r\n]{0,40}?(?:device|adapter|graphics card)[^\r\n:]{0,20}:\s*([^\r\n(,]{4,80})", RegexOptions.IgnoreCase);
                if (m.Success && !p.ContainsKey("gpu")) p["gpu"] = Short(m.Groups[2].Value, 120);
                var r = Regex.Match(log, @"renderer:\s*(vulkan|d3d11|direct3d 11|metal|d3d12)", RegexOptions.IgnoreCase);
                if (r.Success) p["renderer"] = r.Groups[1].Value.ToLowerInvariant().Replace("direct3d 11", "d3d11");
            }
            return p;
        }

        static string Short(string s, int n) { s = (s ?? "").Trim(); return s.Length > n ? s.Substring(0, n) : s; }

        /// <summary>this PC's kind: models and sizes only (Windows: the registry's processor name and the display
        /// adapter's class entry; elsewhere the processor count and RAM alone)</summary>
        public static Dictionary<string, object> Pc()
        {
            var p = new Dictionary<string, object>
            {
                ["os"] = Short(System.Runtime.InteropServices.RuntimeInformation.OSDescription, 60),
                ["cpu_threads"] = Environment.ProcessorCount,
                ["ram_gb"] = Math.Round(GC.GetGCMemoryInfo().TotalAvailableMemoryBytes / 1073741824.0, 1),
            };
            if (OperatingSystem.IsWindows())
            {
                try
                {
                    using var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
                    if (k?.GetValue("ProcessorNameString") is string cpu) p["cpu"] = Short(cpu, 120);
                }
                catch { }
                try
                {   // (the display adapters' class: the first entry with a driver description that is not a basic or remote one)
                    using var cls = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
                    foreach (string n in cls?.GetSubKeyNames() ?? Array.Empty<string>())
                    {
                        if (n.Length != 4) continue;
                        try
                        {
                            using var a = cls.OpenSubKey(n);
                            if (!(a?.GetValue("DriverDesc") is string desc) || desc.Contains("Basic", StringComparison.OrdinalIgnoreCase) || desc.Contains("Remote", StringComparison.OrdinalIgnoreCase)) continue;
                            p["gpu"] = Short(desc, 120);
                            if (a.GetValue("DriverVersion") is string dv) p["gpu_driver"] = Short(dv, 60);
                            break;
                        }
                        catch { }
                    }
                }
                catch { }
            }
            return p;
        }

        /// <summary>after a game: true when its summary was sent. Every outcome goes to log. The file is removed once
        /// looked at (sent or not): a summary is sent once at most.</summary>
        public static async Task<bool> SendIfAnyAsync(Account account, string root, DateTime startedUtc, string commit, Action<string> log)
        {
            string file = Path.Combine(root, "reports", FileName);
            try
            {
                if (!File.Exists(file) || File.GetLastWriteTimeUtc(file) <= startedUtc) return false;
                string json = File.ReadAllText(file);
                try { File.Delete(file); } catch { }
                if (!Enabled(root)) { log("performance summary: off in the settings"); return false; }
                bool test = Environment.GetEnvironmentVariable("OPENSMASH_PERF_TEST") == "1";
                if (!test && (account == null || !account.SignedIn)) { log("performance summary: not signed in, not sent"); return false; }
                string playLog = Path.Combine(root, "build", "play.log");
                string logText = File.Exists(playLog) ? CrashReport.Tail(playLog, 256 * 1024) : null;
                var p = Payload(json, Pc(), logText, Updater.Current.ToString(), commit);
                if (p == null) { log("performance summary: too short a session, not sent"); return false; }
                string body = JsonSerializer.Serialize(new Dictionary<string, object> { ["p"] = p, ["p_product"] = "opensmash" });
                if (test)
                {
                    File.WriteAllText(Path.Combine(root, "build", "perf_test_payload.json"), body);
                    log("performance summary (test): written, not sent");
                    return false;
                }
                await account.RestAsync("POST", "/rest/v1/rpc/perf_session_add", body);
                log("performance summary sent (" + string.Format(CultureInfo.InvariantCulture, "{0:0} s", p.TryGetValue("session_s", out var s) ? s : 0) + ")");
                return true;
            }
            catch (Exception ex) { log("performance summary not sent: " + ex.Message); return false; }
        }
    }
}
