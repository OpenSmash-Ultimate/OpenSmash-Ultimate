// SPDX-License-Identifier: GPL-3.0-or-later
// After a game crash: the mod whose plugin the crash happened in, from the report's first line, for a suggestion to
// turn that mod off. The runtime (runtime/host.c crash_blame, SSBU_CRASH_BLAME) ends that line with
// " - in plugin <module> (<module>+0x...)" when the fault is in a Skyline plugin's code, or
// " - in code called from plugin <module> (...)" when a plugin called the code that faulted; only the innermost two
// guest frames count. dump\skyline\plugins.json (tools/skyline.py's record) says which mod each plugin module came from.
// Field reports of 0.5.1-0.5.3 (26-27/11) had a mod's plugin there in about one crash in five (a moveset's plugin, a
// mod menu); before, the player only saw that the game stopped.
// A known crash (runtime/crash_blame.h known_crash, SSBU_CRASH_BLAME_KNOWN): the line ends " - with plugin <module> (a
// known crash with it loaded: main+0x...)" when the faulting frames are all the game's but field reports show the crash
// only with that plugin loaded: The CSK Collection's null read as a match loads or right after one, about 45% of 0.5.4's
// crash reports. The CSK Collection is a standard plugin (tools/std_plugins.py), not a mod of its own: the mod offered is
// then the one that adds characters through it (dump\skyline\csk.json, the runtime's record of them) when only one does;
// several are named.
// OPENSMASH_CRASH_MOD_HINT=0 (or off): no suggestion, as before.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace OpenSmash.Core
{
    public static class CrashBlame
    {
        public const string HintVariable = "OPENSMASH_CRASH_MOD_HINT";

        /// <summary>what the report's first line says about a plugin: its module, whether the fault was in its own code
        /// (false: in code it called, or a known crash), and whether it is a known crash with the plugin loaded
        /// (" - with plugin X (a known crash ...)"); null when it names none</summary>
        public static (string Module, bool Own, bool Known)? Plugin(string report)
        {
            if (string.IsNullOrEmpty(report)) return null;
            string first = report.Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0) ?? "";
            if (!first.StartsWith("*** host exception", StringComparison.Ordinal)) return null;
            var m = Regex.Match(first, @" - in (code called from )?plugin ([A-Za-z0-9_.\-]+) \(");
            if (m.Success) return (m.Groups[2].Value, !m.Groups[1].Success, false);
            m = Regex.Match(first, @" - with plugin ([A-Za-z0-9_.\-]+) \(a known crash");
            return m.Success ? (m.Groups[1].Value, false, true) : null;
        }

        /// <summary>the mods (not the standard plugins) whose plugins added characters through The CSK Collection
        /// (dump\skyline\csk.json: each entry's "plugin" module, its mod from plugins.json), in the json's order</summary>
        public static List<string> CskUsers(string root)
        {
            var mods = new List<string>();
            try
            {
                string json = Path.Combine(root, "dump", "skyline", "csk.json");
                if (!File.Exists(json)) return mods;
                using var doc = JsonDocument.Parse(File.ReadAllText(json));
                if (doc.RootElement.ValueKind != JsonValueKind.Array) return mods;
                foreach (var e in doc.RootElement.EnumerateArray())
                {
                    if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty("plugin", out var p) || p.ValueKind != JsonValueKind.String) continue;
                    var mod = ModOf(root, p.GetString());
                    if (mod != null && !mod.Value.Standard && !mods.Contains(mod.Value.Mod)) mods.Add(mod.Value.Mod);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { }
            return mods;
        }

        /// <summary>the mod a plugin module came from (dump\skyline\plugins.json), and whether it is one of the standard
        /// plugins every modded game loads (ARCropolis, Smashline...: "(standard) ..."); null if not listed</summary>
        public static (string Mod, bool Standard)? ModOf(string root, string module)
        {
            if (string.IsNullOrEmpty(module)) return null;
            try
            {
                string record = Path.Combine(root, "dump", "skyline", "plugins.json");
                if (!File.Exists(record)) return null;
                using var doc = JsonDocument.Parse(File.ReadAllText(record));
                if (doc.RootElement.ValueKind != JsonValueKind.Object) return null;
                (string, bool)? found = null;
                // (exactly its module; else the same name without the "_" a second copy of a plugin gets, the_csk_collection_)
                foreach (bool exact in new[] { true, false })
                {
                    foreach (var e in doc.RootElement.EnumerateObject())
                    {
                        if (e.Value.ValueKind != JsonValueKind.Object) continue;
                        string mod = e.Value.TryGetProperty("mod", out var md) && md.ValueKind == JsonValueKind.String ? md.GetString() : null;
                        string name = e.Value.TryGetProperty("module", out var mo) && mo.ValueKind == JsonValueKind.String ? mo.GetString() : null;
                        if (string.IsNullOrEmpty(mod) || string.IsNullOrEmpty(name)) continue;
                        bool same = exact ? name == module : name.TrimEnd('_') == module.TrimEnd('_');
                        if (!same) continue;
                        bool standard = mod.StartsWith("(standard)", StringComparison.Ordinal);
                        if (!standard) return (mod, false);   // (a mod's plugin before a standard one of the same name)
                        found ??= (mod, true);
                    }
                    if (found != null) return found;
                }
                return null;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return null; }
        }

        /// <summary>OPENSMASH_CRASH_MOD_HINT: on unless 0 or off</summary>
        public static bool HintOn(string value)
        {
            string v = (value ?? "").Trim();
            return !(v == "0" || v.Equals("off", StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>After a game: the newest crash report written since it started, and what it says. Mod: the mod to suggest
        /// turning off (null: none - no plugin named, a standard plugin, or not listed); Text: the message to show (null:
        /// nothing to say). With OPENSMASH_CRASH_MOD_HINT=0: nothing.</summary>
        public static (string Mod, string Text) Hint(string root, DateTime startedUtc)
        {
            if (!HintOn(Environment.GetEnvironmentVariable(HintVariable))) return (null, null);
            string file = CrashReport.NewReport(root, startedUtc);
            if (file == null || !Path.GetFileName(file).StartsWith("crash_", StringComparison.OrdinalIgnoreCase)) return (null, null);
            string head;
            try { head = CrashReport.HeadAndTail(file, 4096, 4096); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (null, null); }
            return HintFor(root, head);
        }

        /// <summary>Hint for a report's text (tests)</summary>
        public static (string Mod, string Text) HintFor(string root, string report)
        {
            var p = Plugin(report);
            if (p == null) return (null, null);
            var mod = ModOf(root, p.Value.Module);
            if (p.Value.Known)
            {
                string known = "The game crashed in a way seen so far only with the plugin " + p.Value.Module + " loaded (in the game's own code, as a match loads or right after one)";
                if (mod != null && !mod.Value.Standard)
                    return (mod.Value.Mod, known + ", from the mod \"" + mod.Value.Mod + "\". If it happens again, turn that mod off in Mods.");
                var users = p.Value.Module.TrimEnd('_') == "the_csk_collection" ? CskUsers(root) : new List<string>();
                if (users.Count == 1)
                    return (users[0], known + ". The mod \"" + users[0] + "\" adds characters through it. If it happens again, turn that mod off in Mods.");
                if (users.Count > 1)
                    return (null, known + ". These mods add characters through it: " + string.Join(", ", users.Take(6).Select(u => "\"" + u + "\"")) +
                                  (users.Count > 6 ? " and " + (users.Count - 6) + " more" : "") + ". If it happens again, try the game with some of them turned off.");
                return (null, known + ". If it happens again, try turning off the mods that add new characters.");
            }
            string where = p.Value.Own ? "in the plugin " + p.Value.Module : "in the game's code, called from the plugin " + p.Value.Module;
            if (mod == null)
                return (null, "The game crashed " + where + ". If it happens again, try the game with the mod that installs this plugin turned off.");
            if (mod.Value.Standard)   // (on nearly every modded PC: the mod that uses it is more likely the cause, and which one is not known)
                return (null, "The game crashed " + where + ", one of the plugins every modded game loads. If it happens again, try turning off the mods you added most recently.");
            return (mod.Value.Mod, "The game crashed " + where + ", from the mod \"" + mod.Value.Mod + "\". If it happens again, turn that mod off in Mods.");
        }
    }
}
