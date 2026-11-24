// SPDX-License-Identifier: GPL-3.0-or-later
// The three versions a player is asked for in a report: the launcher's (Updater.Current), the OpenSmash commit this
// folder's sources are at (tools/source_version.txt, or the git checkout's HEAD), and the game's (version.txt,
// tools/build_game.py). The launchers show them in their UPDATE card; the game's Esc menu (runtime/gpu_menu.c, Info)
// shows them too, the launcher's from OPENSMASH_LAUNCHER_VERSION, which the launchers set when they start it.
using System;
using System.IO;
using System.Linq;

namespace OpenSmash.Core
{
    public static class VersionInfo
    {
        public static string Launcher => Updater.Current.ToString();

        static string FirstLine(string path)
        {
            try { return File.Exists(path) ? (File.ReadLines(path).FirstOrDefault() ?? "").Trim() : ""; }
            catch (IOException) { return ""; }
            catch (UnauthorizedAccessException) { return ""; }
        }

        static bool Hex40(string t) => t != null && t.Length >= 40 && t.Take(40).All(Uri.IsHexDigit);

        /// <summary>the game version of the folder (version.txt's first line), "" when unknown</summary>
        public static string Game(string root) => string.IsNullOrEmpty(root) ? "" : FirstLine(Path.Combine(root, "version.txt"));

        /// <summary>the sources' commit, short, with its date when known ("abc1234 2026-11-24"); a git checkout's HEAD
        /// ("abc1234 (git)"); "" when unknown</summary>
        public static string Source(string root)
        {
            if (string.IsNullOrEmpty(root)) return "";
            string line = FirstLine(Path.Combine(root, "tools", "source_version.txt"));
            if (Hex40(line)) return (line.Substring(0, 7) + " " + line.Substring(40).Trim()).Trim().ToLowerInvariant();
            string head = FirstLine(Path.Combine(root, ".git", "HEAD"));
            if (Hex40(head)) return head.Substring(0, 7).ToLowerInvariant() + " (git)";
            if (head.StartsWith("ref: refs/heads/", StringComparison.Ordinal))
            {
                string sha = FirstLine(Path.Combine(root, ".git", head.Substring(5).Replace('/', Path.DirectorySeparatorChar)));
                return Hex40(sha) ? sha.Substring(0, 7).ToLowerInvariant() + " (git, " + head.Substring(16) + ")" : "git, " + head.Substring(16);
            }
            return "";
        }

        /// <summary>the line the launchers show: "LAUNCHER 0.5.0 · RECOMP abc1234 2026-11-24 · GAME 13.0.5"</summary>
        public static string Line(string root)
        {
            string s = Source(root), g = Game(root);
            return "LAUNCHER " + Launcher + "   ·   RECOMP " + (s.Length > 0 ? s : "?") + "   ·   GAME " + (g.Length > 0 ? g : "?");
        }

        /// <summary>the environment variable the launchers give the game at Play: their version, for its Esc menu</summary>
        public const string EnvName = "OPENSMASH_LAUNCHER_VERSION";
    }
}
