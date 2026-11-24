// SPDX-License-Identifier: GPL-3.0-or-later
// The Online settings' rules, the same in every launcher (WinForms, Avalonia; tools/weblauncher/launcher.py mirrors them):
// - the official lobby ("auto": the server region's official lobby, relay only) is the default: an online_server.txt that
//   is missing or has no line is made "auto" (EnsureLine). A line the player chose (offline, LAN, a private code, a server
//   of one's own) is kept as it is;
// - the official servers (the official lobby, and the private lobby: the official lobby with a code) are relay only:
//   "Direct connections" is hidden there and written "off"; the official lobby has no lobby code and no server of one's
//   own: both are hidden AND reset (in the page and in launcher.ini's remembered copy), so no stale code or address can be
//   sent; the private lobby has no server of one's own (reset too);
// - leaving the official servers for a server of one's own or the LAN shows those rows again: the code and the server
//   empty, Direct connections back to the game's default ("on");
// - the server region is one of the official regions (Europe, North America East, South America): "auto" in server_region.txt (the
//   game's default: the closest) is shown as the region it picks and written only when the player picks one.
// Keys of the values: "lobby" (auto, private, custom, lan, offline), "code", "server", "direct" (on/off), "region".
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace OpenSmash.Core
{
    public static class OnlineSettings
    {
        public const string Official = "auto";
        public const string DirectDefault = "on";   // (online_direct.txt's default, runtime/net_relay.c)

        /// <summary>the lobby choices that play on an official server (relay only)</summary>
        public static bool OfficialServer(string lobby) => lobby == Official || lobby == "private";

        /// <summary>a server of one's own as the game takes it: host:port</summary>
        public static bool ValidServer(string host) => Regex.IsMatch((host ?? "").Trim(), @"^[^\s:]+:\d{1,5}$");

        /// <summary>whether a row of the Online tab is shown for this lobby choice (regionOverride: an install that names its
        /// own official server has no regions)</summary>
        public static bool Shown(string key, string lobby, bool regionOverride = false) => key switch
        {
            "name" => lobby != "offline",
            "code" => lobby == "private" || lobby == "custom",
            "server" => lobby == "custom",
            "direct" => lobby == "custom" || lobby == "lan",
            "region" => OfficialServer(lobby) && !regionOverride,
            _ => true,
        };

        static bool Set(IDictionary<string, string> v, string key, string value, ICollection<string> changed)
        {
            if (v.TryGetValue(key, out var old) && old == value) return false;
            v[key] = value;
            if (!changed.Contains(key)) changed.Add(key);
            return true;
        }

        /// <summary>what the lobby choice makes unused, reset (see above); the keys that changed (to be written)</summary>
        public static List<string> Normalize(IDictionary<string, string> v)
        {
            var changed = new List<string>();
            string lobby = v.TryGetValue("lobby", out var l) ? l : Official;
            if (lobby == Official) Set(v, "code", "", changed);
            if (OfficialServer(lobby))
            {
                Set(v, "server", "", changed);
                Set(v, "direct", "off", changed);
            }
            return changed;
        }

        /// <summary>a new lobby choice: the value set and what it makes unused reset; leaving the official servers for a
        /// server of one's own or the LAN brings Direct connections back to its default. The keys that changed.</summary>
        public static List<string> Choose(IDictionary<string, string> v, string lobby, string before = null)
        {
            before ??= v.TryGetValue("lobby", out var l) ? l : Official;   // (the choice the page showed before, when the caller knows it)
            var changed = new List<string>();
            Set(v, "lobby", lobby, changed);
            if (!changed.Contains("lobby")) changed.Add("lobby");
            foreach (var k in Normalize(v)) if (!changed.Contains(k)) changed.Add(k);
            if (OfficialServer(before) && !OfficialServer(lobby) && lobby != "offline") Set(v, "direct", DirectDefault, changed);
            return changed;
        }

        /// <summary>null when the choice can be written for the game; else what is missing ("code": a private lobby needs a
        /// code; "server": a server of one's own needs host:port)</summary>
        public static string Missing(string lobby, string code, string host)
        {
            if (lobby == "private" && string.IsNullOrWhiteSpace(code)) return "code";
            if (lobby == "custom" && !ValidServer(host)) return "server";
            return null;
        }

        /// <summary>the region the row shows: a region's id as saved, else (auto, nothing, an unknown word) the region Auto
        /// plays on (its last pick, else the first: Europe)</summary>
        public static string RegionShown(string root)
        {
            string saved = ServerRegion.Setting(root);
            if (saved != ServerRegion.Auto) return saved;
            return ServerRegion.AutoPick(root).Region?.Id ?? ServerRegion.All[0].Id;
        }

        /// <summary>the official lobby when online_server.txt is missing or has no line (comments kept); true when written.
        /// Never throws.</summary>
        public static bool EnsureLine(string serverFile)
        {
            try
            {
                var lines = File.Exists(serverFile) ? File.ReadAllLines(serverFile).ToList() : new List<string>();
                if (lines.Any(x => x.Trim() != "" && !x.TrimStart().StartsWith("#"))) return false;
                lines.Add(Official);
                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(serverFile)));
                File.WriteAllLines(serverFile, lines);
                return true;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException || e is NotSupportedException) { return false; }
        }
    }
}
