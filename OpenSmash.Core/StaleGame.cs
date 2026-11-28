// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.5) "The launcher updates but the game does not": six PCs in the crash reports and issues #639, #599, #603 ran a
// launcher of one release with the game of another (0.5.4's launcher with 0.5.3's sources and build; one 0.5.1 launcher with
// 0.5.4's). Before Play the launchers now compare what they know without asking anyone:
//  - this launcher's version and the release it belongs to (the release manifest the update check fetched) against the
//    sources' commit (tools/source_version.txt): the launcher is the release's and the sources are not ("sources_behind"),
//    or the sources are the release's and the launcher is older ("launcher_older");
//  - the build's own record (build/build_commit.txt, written by the build since v0.5.5's build commit change, PR #642:
//    "commit <sha>", "current <sha>...") against the sources' commit: the game was built from other sources and was not
//    found current for these ("not_rebuilt"). Without that record nothing is said (a build's date alone cannot tell: an
//    update that changes nothing of the game does not build it again).
// When one is found the player is told what to press, and a stale_game report goes (SetupReport, its switches and limits).
// OPENSMASH_STALE_CHECK=0: no check, as before. A developer's git checkout and a local launcher (0.0.0) are never checked
// against a release.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace OpenSmash.Core
{
    public static class StaleGame
    {
        public const string Variable = "OPENSMASH_STALE_CHECK";
        public static bool On => SetupReport.SwitchOn(Environment.GetEnvironmentVariable(Variable));

        public sealed class Finding
        {
            /// <summary>sources_behind | launcher_older | not_rebuilt</summary>
            public string Reason;
            /// <summary>what the player reads (what to press)</summary>
            public string Message;
            /// <summary>the report's error line: the versions and commits compared</summary>
            public string Detail;
        }

        static bool Sha(string s) => s != null && s.Length == 40 && s.All(Uri.IsHexDigit);
        static string Short(string s) => string.IsNullOrEmpty(s) ? "?" : s.Length > 10 ? s.Substring(0, 10) : s;

        /// <summary>build/build_commit.txt: its keys ("current" may come more than once: joined), null when there is none</summary>
        public static Dictionary<string, string> Record(string root)
        {
            try
            {
                string f = Path.Combine(root, "build", "build_commit.txt");
                if (!File.Exists(f)) return null;
                var d = new Dictionary<string, string>(StringComparer.Ordinal);
                foreach (string raw in File.ReadAllLines(f))
                {
                    string line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#') continue;
                    int sp = line.IndexOf(' ');
                    string key = sp < 0 ? line : line.Substring(0, sp), value = sp < 0 ? "" : line.Substring(sp + 1).Trim();
                    d[key] = key == "current" && d.TryGetValue("current", out var was) ? was + " " + value : value;
                }
                return d;
            }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }

        /// <summary>the commit the build says it was made from, null when not recorded</summary>
        public static string RecordedCommit(string root)
        {
            var r = string.IsNullOrEmpty(root) ? null : Record(root);
            return r != null && r.TryGetValue("commit", out var c) && Sha(c.ToLowerInvariant()) ? c.ToLowerInvariant() : null;
        }

        /// <summary>the check itself, on what is given: launcher (this launcher's version), releaseVersion / releaseCommit (the
        /// release this channel's update check found; null when it is not known), sources (tools/source_version.txt's commit),
        /// record (build/build_commit.txt's keys or null). null: nothing wrong found.</summary>
        public static Finding Check(SemVer launcher, SemVer releaseVersion, string releaseCommit, string sources, Dictionary<string, string> record)
        {
            sources = sources?.ToLowerInvariant();
            releaseCommit = releaseCommit?.ToLowerInvariant();
            bool localLauncher = launcher == null || (launcher.Major == 0 && launcher.Minor == 0 && launcher.Patch == 0);
            if (!localLauncher && releaseVersion != null && Sha(releaseCommit) && Sha(sources))
            {
                int c = launcher.CompareTo(releaseVersion);
                if (c >= 0 && launcher.Major == releaseVersion.Major && launcher.Minor == releaseVersion.Minor && launcher.Patch == releaseVersion.Patch
                    && sources != releaseCommit)
                    return new Finding
                    {
                        Reason = "sources_behind",
                        Message = "The launcher was updated to " + launcher + ", but the game was not: it is still another version's (" + Short(sources).Substring(0, 7) + "). Press Update to bring the game up to date and rebuild it.",
                        Detail = "stale game: launcher " + launcher + " (release " + releaseVersion + " " + Short(releaseCommit) + "), sources " + Short(sources),
                    };
                if (c < 0 && sources == releaseCommit)
                    return new Finding
                    {
                        Reason = "launcher_older",
                        Message = "This launcher (" + launcher + ") is older than the game (" + releaseVersion + "). Press Update to get the new launcher.",
                        Detail = "stale launcher: launcher " + launcher + ", sources " + Short(sources) + " are release " + releaseVersion,
                    };
            }
            if (record != null && Sha(sources) && record.TryGetValue("commit", out var built) && Sha(built?.ToLowerInvariant()))
            {
                built = built.ToLowerInvariant();
                bool current = record.TryGetValue("current", out var list) && list.Split(new[] { ' ', ',' }, StringSplitOptions.RemoveEmptyEntries)
                                                                                     .Any(x => x.Equals(sources, StringComparison.OrdinalIgnoreCase));
                if (built != sources && !current)
                    return new Finding
                    {
                        Reason = "not_rebuilt",
                        Message = "The game was not rebuilt after the update: it was built from " + built.Substring(0, 7) + " and the sources are " + sources.Substring(0, 7) + ". Press Update (or Repair in Settings) to build it again.",
                        Detail = "stale game: built from " + Short(built) + ", sources " + Short(sources),
                    };
            }
            return null;
        }

        /// <summary>the check of an install (root): null when off, a git checkout, or nothing wrong</summary>
        public static Finding Check(string root, Release release)
        {
            if (!On || string.IsNullOrEmpty(root) || Updater.DevCopy(root)) return null;
            return Check(Updater.Current, release?.Version, release?.Commit, Updater.InstalledCommit(root), Record(root));
        }

        /// <summary>a stale_game report of a finding</summary>
        public static SetupFailure Failure(string root, Finding f) => new SetupFailure
        {
            Kind = "stale_game", Where = "play: " + f.Reason, Error = f.Detail, Root = root,
        };

        /// <summary>(game_start_error) a game that could not start: its log says it could not use its game library (runtime/host.c
        /// game_refused: "[boot] cannot use the game library ...", a LoadLibrary error such as 126 among them, #599), or it ended
        /// with a code other than 0 before writing a single line. Those games write no crash report. null: neither.</summary>
        public static SetupFailure StartFailure(string root, string log, int? exitCode)
        {
            string text = "";
            try { if (!string.IsNullOrEmpty(log) && File.Exists(log)) text = CrashReport.HeadAndTail(log, 64 * 1024, 32 * 1024); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            var lines = text.Replace("\r", "").Split('\n');
            int at = Array.FindIndex(lines, l => l.Contains("cannot use the game library", StringComparison.OrdinalIgnoreCase));
            if (at >= 0)
            {
                string line = lines[at].Trim();
                var err = System.Text.RegularExpressions.Regex.Match(line, @"\(error (\d+)\)");
                return new SetupFailure
                {
                    Kind = "game_start_error", Root = root, ExitCode = exitCode, Logs = new[] { log },
                    Where = "game start: game library" + (err.Success ? ", LoadLibrary error " + err.Groups[1].Value : ""),
                    Error = line + (at + 1 < lines.Length && lines[at + 1].Trim().Length > 0 ? "\n" + lines[at + 1].Trim() : ""),
                };
            }
            if (exitCode is int code && code != 0 && lines.All(l => l.Trim().Length == 0))
                return new SetupFailure
                {
                    Kind = "game_start_error", Root = root, ExitCode = code, Where = "game start: no log",
                    Error = "The game ended (exit code " + code + ", 0x" + code.ToString("X8") + ") before writing a line to its log.",
                };
            return null;
        }
    }
}
