// SPDX-License-Identifier: GPL-3.0-or-later
// OpenSmash's official lobby, by its DNS name: the one place the launcher takes it from (the public lobby, the players
// online, the leaderboard, the shown name). Never an address in numbers: the server behind the name can move.
// The game's copy is runtime/net.h's NET_OFFICIAL_LOBBY (tests/test_launcher_defaults.py compares them).
// One port: UDP for play, TCP for the lobby's read-only pages (/status.json, /leaderboard.json, /gsp/...). Those pages
// are plain HTTP: the lobby itself serves no TLS, and they carry no account data (counts, opaque ids, chosen names).
// Which lobby, as the game reads it (runtime/net_relay.c public_lobby): SSBU_OFFICIAL_LOBBY, else the install's
// config/official_lobby.txt (Updater.WriteLobby writes the release's "lobby"), else Default.
using System;
using System.IO;
using System.Linq;

namespace OpenSmash.Core
{
    public static class OfficialLobby
    {
        public const string Host = "eu.lobby.opensmashultimate.com";
        public const int Port = 39421;
        public const string Default = Host + ":39421";
        /// <summary>the public lobbies before the official one, gone: their lines in online_server.txt mean it</summary>
        public static readonly string[] Older = { "opensmash-lobby.duckdns.org", "smash.recomps.wtf" };

        /// <summary>host:port and nothing else (what the release, the file or the variable may say)</summary>
        public static bool Valid(string lobby)
        {
            if (string.IsNullOrEmpty(lobby) || lobby.Length > 200) return false;
            int colon = lobby.LastIndexOf(':');
            if (colon <= 0 || !int.TryParse(lobby.Substring(colon + 1), out int port) || port <= 0 || port > 65535) return false;
            return lobby.Substring(0, colon).All(c => char.IsLetterOrDigit(c) || c == '.' || c == '-') && lobby.Substring(colon + 1).All(char.IsDigit);
        }

        /// <summary>the official lobby of the install in root (null: no install yet)</summary>
        public static string Current(string root = null)
        {
            string e = Environment.GetEnvironmentVariable("SSBU_OFFICIAL_LOBBY")?.Trim();
            if (Valid(e)) return e;
            if (!string.IsNullOrEmpty(root))
            {
                try
                {
                    string f = Path.Combine(root, "config", "official_lobby.txt");
                    if (File.Exists(f))
                    {
                        string line = File.ReadLines(f).Select(l => l.Trim()).FirstOrDefault(l => l != "" && !l.StartsWith("#")) ?? "";
                        string word = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault();
                        if (Valid(word)) return word;
                    }
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
            return Default;
        }

        /// <summary>online_server.txt's first word means the official lobby (runtime/net_relay.c means_public_lobby)</summary>
        public static bool Means(string host, string root = null)
        {
            if (string.IsNullOrEmpty(host)) return false;
            if (new[] { "auto", "na", "eu", Default, Host, Current(root) }.Contains(host, StringComparer.OrdinalIgnoreCase)) return true;
            return Older.Any(o => host.StartsWith(o, StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>a read-only page of the lobby ("/status.json"); http: the lobby serves no TLS</summary>
        public static string Page(string path, string root = null) => "http://" + Current(root) + path;
    }
}
