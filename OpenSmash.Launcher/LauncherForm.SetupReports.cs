// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.5) The launcher window's setup error reports (OpenSmash.Core SetupReport.cs, SetupReports.cs): an update that
// stopped (its download, unpacking, files replaced - a file in use or access denied by an antivirus among them - the
// launcher's own replacement), the game's rebuild after an update or a repair (the end of its output: a standard plugin's
// or ARCropolis' compile error is there), the maintenance build; at Play, a game not rebuilt after an update (StaleGame.cs)
// and a game that could not start (its game library did not load, #599, or it ended before writing a log line). The error
// code goes with the message the player already gets. OPENSMASH_SETUP_REPORTS=0: none; OPENSMASH_STALE_CHECK=0: no check.
using System.Text.RegularExpressions;
using OpenSmash.Core;

namespace OpenSmash.Launcher
{
    partial class LauncherForm
    {
        /// <summary>the last report's first line (its code), for the message that follows it; "" when none</summary>
        string _reportLine = "";
        /// <summary>the current update's failure was reported already (by the rebuild)</summary>
        bool _setupReported;
        /// <summary>a stale game is said once a run</summary>
        bool _staleSaid;

        string ReportSuffix() => _reportLine.Length > 0 ? "\n\n" + _reportLine : "";

        /// <summary>the most telling line of a step's output: its last error line, else its last line</summary>
        internal static string ErrorLine(string output)
        {
            var lines = (output ?? "").Replace("\r", "").Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith("##ssbu ", StringComparison.Ordinal)).ToList();
            string? hit = lines.LastOrDefault(l => Regex.IsMatch(l, @"\b(error|failed|fatal|cannot|could not|stopped|denied|in use)\b", RegexOptions.IgnoreCase)
                                                   && !Regex.IsMatch(l, @"^(LOG percent|\[\d+/\d+\])"));
            return hit ?? lines.LastOrDefault() ?? "";
        }

        /// <summary>sends a report of this window's (flow: update | rebuild | build | stale | game_start); its first line is
        /// _reportLine at once, its result _updateDetail later</summary>
        void ReportSetup(string flow, string where, string error, int? exitCode = null, string? output = null, Exception? ex = null, IEnumerable<string>? logs = null)
        {
            var f = new SetupFailure
            {
                Kind = SetupReport.KindFor(flow, null, flow == "update" ? ErrorLine(output ?? "") + " " + error : null),
                Where = where, Error = error, ExitCode = exitCode, Root = _repo, Output = output, Detail = ex?.ToString(), Logs = logs,
            };
            SetupReports.Send(f, this, line =>
            {
                _reportLine = line;
                if (flow is "update" or "rebuild" && _updateState == "error") { _updateDetail = line; Invalidate(); }
            });
        }

        /// <summary>(BuildGameAfterUpdateAsync) the installer's build step stopped: a build_error with its output</summary>
        void ReportRebuild(int code, string output)
        {
            _setupReported = true;
            ReportSetup("rebuild", "install_opensmash.ps1 -BuildAfterUpdate", "exit " + code + ": " + ErrorLine(output), code, output);
        }

        /// <summary>(ApplyUpdateAsync) the update stopped: an update_error (a build_error when its output says the build
        /// failed), unless the rebuild reported it already</summary>
        void ReportUpdate(string why, Exception? ex, string output)
        {
            if (_setupReported) return;
            _setupReported = true;
            string error = ex != null ? SetupReport.Describe(ex) : SetupReport.FirstLine(why);
            string said = ErrorLine(output);   // (the updater's own error line: "The updater stopped (exit 1)." alone says nothing)
            if (said.Length > 0 && !error.Contains(said, StringComparison.Ordinal)) error += " — " + said;
            ReportSetup("update", "update (" + (ReleaseMode ? "release " + (_release?.Version?.ToString() ?? "?") : "main") + ")", error, null, output, ex);
        }

        /// <summary>(Play) a game not rebuilt after an update, or a launcher not updated with its game: told once a run, and
        /// reported (stale_game). The check reads two small files: no waiting.</summary>
        void CheckStaleGame()
        {
            if (_staleSaid) return;
            StaleGame.Finding? found;
            try { found = StaleGame.Check(_repo, ReleaseMode ? _release : null); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return; }
            if (found == null) return;
            _staleSaid = true;
            Updater.Log(_repo, "stale game: " + found.Detail);
            var f = StaleGame.Failure(_repo, found);
            string first = SetupReports.FirstMessage(f);
            SetupReports.Send(f, this, null);
            UiLocale.Message(this, found.Message + (first.Length > 0 ? "\n\n" + first : ""), "OpenSmash", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }

        /// <summary>(Play) the game ended as it was starting: a game_start_error when it could not use its game library or wrote
        /// nothing; the line to add to the message</summary>
        string ReportGameStart(System.Diagnostics.Process proc)
        {
            int? code = null;
            try { code = proc.ExitCode; } catch (InvalidOperationException) { }
            var f = StaleGame.StartFailure(_repo, Path.Combine(_buildDir, "play.log"), code);
            if (f == null) return "";
            string first = SetupReports.FirstMessage(f);
            SetupReports.Send(f, this, null);
            return first.Length > 0 ? "\n\n" + first : "";
        }
    }
}
