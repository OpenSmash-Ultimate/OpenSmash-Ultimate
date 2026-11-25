// SPDX-License-Identifier: GPL-3.0-or-later
// After a game: a crash, freeze or rollback report it wrote (reports\crash_*.txt, freeze_*.txt, rollback_*.txt in the
// install's folder: runtime/host.c rt_report_path) is sent to the accounts service with the end of build\play.log
// (SSBU Arena's CrashReport: the private bucket "crashes" and public.crash_report_add, with p_product = 'opensmash').
// Only when signed in and with the setting on. Before sending, what identifies the player or the PC is removed (Clean).
// Minidumps (memory contents) are never sent.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public static class CrashReport
    {
        public const int MaxLog = 1024 * 1024, MaxReport = 256 * 1024;   // bytes sent at most of each file
        public const int ReportHead = 64 * 1024;   // of a longer report: its start sent (what happened, "Why:"), then its end
        static readonly string[] Kinds = { "crash", "freeze", "rollback" };

        /// <summary>the newest report written since the game started, or null</summary>
        public static string NewReport(string root, DateTime startedUtc)
        {
            try
            {
                var dir = new DirectoryInfo(Path.Combine(root, "reports"));
                if (!dir.Exists) return null;
                return dir.GetFiles("*.txt").Where(f => Kinds.Any(k => f.Name.StartsWith(k + "_", StringComparison.OrdinalIgnoreCase)) && f.LastWriteTimeUtc > startedUtc)
                          .OrderByDescending(f => f.LastWriteTimeUtc).Select(f => f.FullName).FirstOrDefault();
            }
            catch { return null; }
        }

        /// <summary>what identifies the player or the PC removed from a text (paths, user and PC names, tickets, long
        /// signatures, internet addresses but the rooms' own 10.x, e-mail addresses)</summary>
        public static string Clean(string t, params string[] paths)
        {
            var words = new List<KeyValuePair<string, string>>();
            void Add(string w, string to) { if (!string.IsNullOrEmpty(w) && w.Length >= 3) words.Add(new KeyValuePair<string, string>(w, to)); }
            foreach (var p in paths) Add(p?.TrimEnd('\\', '/'), "<install>");
            Add(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "<home>");
            Add(Environment.UserName, "<user>"); Add(Environment.MachineName, "<pc>");
            foreach (var w in words.OrderByDescending(w => w.Key.Length))
            {
                t = Regex.Replace(t, Regex.Escape(w.Key), w.Value, RegexOptions.IgnoreCase);
                if (w.Key.Contains('\\')) t = Regex.Replace(t, Regex.Escape(w.Key.Replace('\\', '/')), w.Value, RegexOptions.IgnoreCase);
            }
            t = Regex.Replace(t, @"[0-9a-fA-F-]{36}\|[^|\s]{1,64}\|\d+(\|\d+)?\.[0-9a-fA-F]{64}", "<pass>");
            t = Regex.Replace(t, @"\b[0-9a-fA-F]{64}\b", "<signature>");
            t = Regex.Replace(t, @"\b(?!10\.)(?!0\.0\.0\.0\b)(?!127\.0\.0\.1\b)\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b", "<address>");
            t = Regex.Replace(t, @"\b[0-9a-fA-F:]*:[0-9a-fA-F:]*:[0-9a-fA-F:]+\b(?=[^0-9a-fA-F]|$)", m => m.Value.Count(c => c == ':') >= 3 ? "<address>" : m.Value);
            return Regex.Replace(t, @"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", "<email>");
        }

        public static string Tail(string path, int max)
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (fs.Length > max) fs.Seek(-max, SeekOrigin.End);
            var b = new byte[Math.Min(fs.Length, max)];
            int n = fs.Read(b, 0, b.Length);
            return Encoding.UTF8.GetString(b, 0, n);
        }

        /// <summary>the file whole when it has max bytes at most; else its first lines (about head bytes: where a report says
        /// what happened, a rollback report's "Why:"), a line "... N bytes omitted ...", and its last lines, max bytes in all.
        /// (Before, a long report's last max bytes only were sent: a rollback report's kept [rb] lines pushed out its "Why:".)</summary>
        public static string HeadAndTail(string path, int max, int head)
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            long length = fs.Length;
            if (length <= max) return Encoding.UTF8.GetString(Read(fs, 0, (int)length));
            const int markRoom = 64;   // (the line between them, whatever N's digits)
            head = Math.Max(0, Math.Min(head, max - markRoom));
            byte[] a = Read(fs, 0, head);
            int nl = Array.LastIndexOf(a, (byte)'\n');
            int aLen = nl >= 0 ? nl + 1 : a.Length;   // (whole lines, unless the first is longer than head)
            int room = max - aLen - markRoom;
            byte[] b = Read(fs, length - room, room);
            nl = Array.IndexOf(b, (byte)'\n');
            int bStart = nl >= 0 && nl + 1 < b.Length ? nl + 1 : 0;   // (whole lines too)
            long omitted = length - aLen - (b.Length - bStart);
            return Encoding.UTF8.GetString(a, 0, aLen) + "\n... " + omitted.ToString(CultureInfo.InvariantCulture) + " bytes omitted ...\n\n" +
                   Encoding.UTF8.GetString(b, bStart, b.Length - bStart);
        }

        static byte[] Read(FileStream fs, long at, int count)
        {
            var b = new byte[count];
            fs.Seek(at, SeekOrigin.Begin);
            int n = 0, r;
            while (n < count && (r = fs.Read(b, n, count - n)) > 0) n += r;
            if (n < count) Array.Resize(ref b, n);
            return b;
        }

        /// <summary>the report's title: its "Why:" when one is among its first lines (rollback reports), else its first line</summary>
        public static string Title(string report, string kind)
        {
            var lines = report.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0).Take(10).ToList();
            string why = lines.FirstOrDefault(l => l.StartsWith("Why:", StringComparison.Ordinal));
            string first = why != null && why.Length > 4 ? why.Substring(4).Trim() : lines.FirstOrDefault() ?? kind;
            return first.Length > 300 ? first.Substring(0, 300) : first;
        }

        /// <summary>after a game: true when a report was found and sent. Every outcome goes to log.</summary>
        public static async Task<bool> SendIfAnyAsync(Account account, string root, DateTime startedUtc, bool enabled, string commit, Action<string> log)
        {
            string file = NewReport(root, startedUtc);
            if (file == null) return false;
            if (!enabled) { log("crash report: off in the settings"); return false; }
            if (!account.SignedIn) { log("crash report: not signed in, not sent"); return false; }
            try
            {
                string kind = Path.GetFileName(file).Split('_')[0].ToLowerInvariant();
                string report = Clean(HeadAndTail(file, MaxReport, ReportHead), root);
                string playLog = Path.Combine(root, "build", "play.log");
                string logText = File.Exists(playLog) ? Clean(Tail(playLog, MaxLog), root) : null;
                string first = Title(report, kind);
                var gpu = logText == null ? null : Regex.Match(logText, @"(?:Vulkan|D3D11|device)[^\r\n]{0,20}:\s*([^\r\n(]{4,80})");
                string stamp = DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
                string folder = "r/" + account.Me.Id + "/" + stamp + "-os";   // (the bucket's policy: r/<own id>/...; the product is a column)
                string reportPath = folder + "-" + kind + ".txt", logPath = logText == null ? null : folder + "-play.log";
                if (Environment.GetEnvironmentVariable("OPENSMASH_CRASH_TEST") == "1")
                {   // (test: what would be sent, beside the log; nothing goes out)
                    File.WriteAllText(Path.Combine(root, "build", "crash_test_report.txt"), report);
                    if (logText != null) File.WriteAllText(Path.Combine(root, "build", "crash_test_log.txt"), logText);
                    log("crash report (test): written, not sent");
                    return false;
                }
                await PutAsync(account, reportPath, report);
                if (logPath != null) await PutAsync(account, logPath, logText);
                var args = new Dictionary<string, object>
                {
                    ["p_product"] = "opensmash", ["p_version"] = Updater.Current.ToString(), ["p_commit"] = commit,
                    ["p_exception"] = kind + ": " + first, ["p_stack"] = null,
                    ["p_gpu"] = gpu != null && gpu.Success ? gpu.Groups[1].Value.Trim() : null,
                    ["p_windows"] = Environment.OSVersion.VersionString, ["p_log_path"] = logPath, ["p_crash_path"] = reportPath,
                };
                await account.RestAsync("POST", "/rest/v1/rpc/crash_report_add", JsonSerializer.Serialize(args));
                log("crash report sent: " + first);
                return true;
            }
            catch (Exception ex) { log("crash report not sent: " + ex.Message); return false; }
        }

        static async Task PutAsync(Account account, string name, string text)
        {
            using var rq = new HttpRequestMessage(HttpMethod.Post, account.Service.Url + "/storage/v1/object/crashes/" + name)
            { Content = new StringContent(text, Encoding.UTF8, "text/plain") };
            rq.Headers.Add("apikey", account.Service.Key);
            rq.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", await account.AccessTokenAsync());
            using var rs = await Releases.Http.SendAsync(rq);
            if (!rs.IsSuccessStatusCode) throw new AccountException("the report upload was refused (" + (int)rs.StatusCode + ")", (int)rs.StatusCode);
        }
    }
}
