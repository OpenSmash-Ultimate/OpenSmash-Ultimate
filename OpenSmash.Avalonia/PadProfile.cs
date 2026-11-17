using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace OpenSmash.Launcher
{
    // controllers.ini's [device ...] sections, read and written the way the game reads them (runtime/hle_pad.c):
    // a section's name is taken up to its ']' before any comment (pads' names carry a '#', "XInput Controller #1"),
    // runs of spaces count as one, and a pad takes [device default], then its type's section, its name's and its
    // GUID's, each later one changing only the keys it has. The remap page writes [device <GUID>]: that model of pad
    // on any port, whatever number Windows gives its name.
    static class PadProfile
    {
        // The console's buttons, as controllers.ini names them (runtime/hle_pad.c NPAD_BUTTON_NAMES), in the page's order.
        public static readonly string[] Buttons = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LSTICK", "RSTICK", "UP", "DOWN", "LEFT", "RIGHT" };

        // What a pad has, as SDL names it (runtime/hle_pad.c RAW_BUTTON_NAMES, in SDL_GamepadButton order, then the triggers).
        public static readonly string[] Sources = {
            "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
            "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "rightpaddle1", "leftpaddle1", "rightpaddle2",
            "leftpaddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "lefttrigger", "righttrigger" };

        // Each console button's control when nothing remaps it (layout = positional, the file's default).
        public static readonly Dictionary<string, string> Positional = new(StringComparer.OrdinalIgnoreCase)
        {
            ["A"] = "east", ["B"] = "south", ["X"] = "north", ["Y"] = "west", ["L"] = "leftshoulder", ["R"] = "rightshoulder",
            ["ZL"] = "lefttrigger", ["ZR"] = "righttrigger", ["PLUS"] = "start", ["MINUS"] = "back", ["LSTICK"] = "leftstick",
            ["RSTICK"] = "rightstick", ["UP"] = "dpup", ["DOWN"] = "dpdown", ["LEFT"] = "dpleft", ["RIGHT"] = "dpright",
        };

        public static readonly string[] Switches = { "dpad_moves", "stick_taunts" };

        public static string SectionFor(string guid) => "device " + guid;

        // A section header's name with its spaces made one, or null when the line is not a header.
        public static string? Header(string line)
        {
            string t = line.Trim();
            if (!t.StartsWith("[")) return null;
            int end = t.IndexOf(']');
            string inner = end < 0 ? t.Substring(1) : t.Substring(1, end - 1);
            return string.Join(' ', inner.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries));
        }

        public static bool SameName(string a, string b) =>
            string.Join(' ', a.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
                .Equals(string.Join(' ', b.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)), StringComparison.OrdinalIgnoreCase);

        // A key's line: (key, value) with a comment after the value cut (';', or '#' followed by a space), or null.
        static (string Key, string Value)? KeyValue(string line)
        {
            string t = line.Trim();
            if (t.Length == 0 || t[0] == ';' || t[0] == '#' || t[0] == '[') return null;
            for (int i = 1; i < t.Length; i++)
                if ((t[i] == ';' || (t[i] == '#' && (i + 1 == t.Length || t[i + 1] == ' ' || t[i + 1] == '\t'))) && (t[i - 1] == ' ' || t[i - 1] == '\t'))
                {
                    t = t.Substring(0, i).TrimEnd();
                    break;
                }
            int eq = t.IndexOf('=');
            if (eq < 1) return null;
            return (t.Substring(0, eq).Trim(), t.Substring(eq + 1).Trim());
        }

        // Every section's keys: name -> key -> value (a repeated key's last, as the game reads it).
        public static Dictionary<string, Dictionary<string, string>> Sections(string text)
        {
            var all = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
            string section = "";
            foreach (string raw in (text ?? "").Split('\n'))
            {
                string? h = Header(raw);
                if (h != null) { section = h; continue; }
                var kv = KeyValue(raw);
                if (kv == null) continue;
                if (!all.TryGetValue(section, out var keys)) all[section] = keys = new(StringComparer.OrdinalIgnoreCase);
                keys[kv.Value.Key] = kv.Value.Value;
            }
            return all;
        }

        // The [device ...] sections a pad takes, from the least exact: default, its type's, its name's, its GUID's.
        public static List<(string Section, Dictionary<string, string> Keys)> Layers(string text, string guid, string name, string type)
        {
            var all = Sections(text);
            var layers = new List<(string, Dictionary<string, string>)>();
            void Add(string what)
            {
                if (what == "") return;
                foreach (var kv in all)
                    if (kv.Key.StartsWith("device ", StringComparison.OrdinalIgnoreCase) && SameName(kv.Key.Substring(7), what))
                    {
                        layers.Add((kv.Key, kv.Value));
                        return;
                    }
            }
            Add("default"); Add(type); Add(name); Add(guid);
            return layers;
        }

        // A console button's control(s) for this pad, and the section that sets it ("" none: the default control).
        public static (string Value, string From) Effective(string text, string guid, string name, string type, string key)
        {
            (string, string) found = (Positional.TryGetValue(key, out var p) ? p : key is "dpad_moves" or "stick_taunts" ? "off" : "", "");
            foreach (var (section, keys) in Layers(text, guid, name, type))
                if (keys.TryGetValue(key, out var v)) found = (v, section);
            return found;
        }

        // The file with [section]'s key set to value (null: the key removed). Other lines, comments and sections stay
        // as they are; a missing section is added at the end with a comment naming the pad.
        public static string Set(string text, string section, string key, string? value, string comment = "")
        {
            string nl = text.Contains("\r\n") ? "\r\n" : "\n";
            var lines = (text ?? "").Replace("\r\n", "\n").Split('\n').ToList();
            if (lines.Count > 0 && lines[^1] == "") lines.RemoveAt(lines.Count - 1);
            int start = -1, end = lines.Count;
            for (int i = 0; i < lines.Count; i++)
            {
                string? h = Header(lines[i]);
                if (h == null) continue;
                if (start >= 0) { end = i; break; }
                if (SameName(h, section)) start = i;
            }
            if (start < 0)
            {
                if (value == null) return text ?? "";
                if (lines.Count > 0 && lines[^1].Trim() != "") lines.Add("");
                lines.Add("[" + section + "]");
                if (comment != "") lines.Add("; " + comment.Replace("\n", " ").Replace("\r", " "));
                lines.Add(key + " = " + value);
                return string.Join(nl, lines) + nl;
            }
            int at = -1, last = start;
            for (int i = start + 1; i < end; i++)
            {
                var kv = KeyValue(lines[i]);
                if (kv == null) continue;
                last = i;
                if (kv.Value.Key.Equals(key, StringComparison.OrdinalIgnoreCase)) { at = i; break; }
            }
            if (at >= 0)
            {
                if (value == null) lines.RemoveAt(at);
                else lines[at] = key + " = " + value;
            }
            else if (value != null)
            {
                if (last == start) while (last + 1 < end && lines[last + 1].TrimStart().StartsWith(";")) last++;   // (after the header's comment)
                lines.Insert(last + 1, key + " = " + value);
            }
            return string.Join(nl, lines) + nl;
        }

        // The first control newly pressed between two reads of a pad (the remap page's capture), or null.
        public static string? NewlyPressed(ISet<string> before, ISet<string> now)
        {
            foreach (string s in Sources)
                if (now.Contains(s) && !before.Contains(s)) return s;
            return null;
        }

        public static string SourceLabel(string source) => source switch
        {
            "south" => "Bottom face button", "east" => "Right face button", "west" => "Left face button", "north" => "Top face button",
            "leftshoulder" => "Left bumper", "rightshoulder" => "Right bumper", "lefttrigger" => "Left trigger", "righttrigger" => "Right trigger",
            "start" => "Start / Menu", "back" => "Back / View", "guide" => "Home / Guide", "leftstick" => "Left stick click", "rightstick" => "Right stick click",
            "dpup" => "D-pad up", "dpdown" => "D-pad down", "dpleft" => "D-pad left", "dpright" => "D-pad right", "none" => "Nothing",
            "touchpad" => "Touchpad click", "misc1" => "Share / Capture",
            _ => source,
        };

        public static string ButtonLabel(string button) => button switch
        {
            "PLUS" => "+ (Start)", "MINUS" => "- (Select)", "LSTICK" => "Left stick click", "RSTICK" => "Right stick click",
            "UP" => "D-pad up", "DOWN" => "D-pad down", "LEFT" => "D-pad left", "RIGHT" => "D-pad right",
            _ => button,
        };
    }
}
