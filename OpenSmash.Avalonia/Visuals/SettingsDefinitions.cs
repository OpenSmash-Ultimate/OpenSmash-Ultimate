// Original Windows launcher definitions, now maintained in the shared implementation.
#nullable disable
#pragma warning disable CS0649 // Original view-model fields include optional mod variants.
using System.Text;
using OpenSmash.Launcher;
namespace OpenSmash.Visuals;
    class ModInfo
    {
        public string Name;
        public string Title;
        public long Size;
        public int Files;
        public bool Plugin;
        public bool Enabled = true;
        public string GamebananaId;
        public string Group;
        public string Part;
        public string GroupTitle;
        public string Author = "";
        public string Category = "";
        public string About = "";
        public string Online = "";
        public bool NewSlots;
        public string Code = "";
        public bool Pick;
        public string Variant = "";
        public int Added, Merged, Skipped, KeptShared;
    }

    class ModLine
    {
        public bool Header;
        public bool Child;
        public bool Open;
        public bool Pick;
        public bool Enabled;
        public bool Mixed;
        public string Title;
        public string Sub;
        public int Files;
        public string Thumb;
        public string GroupKey = "";
        public string[] Names = Array.Empty<string>();
        public string[] Variants = Array.Empty<string>();
        public int Active;
        public string Meta = "";
        public string About = "";
        public string Online = "";
        public bool NewSlots;
        public string Code = "";
        public int Added, Merged, Skipped, KeptShared;
        public int RowH = 68;
        public bool HasChips => Online.Length > 0 || NewSlots || Code.Length > 0 || Added > 0 || Merged > 0 || Skipped > 0 || KeptShared > 0;
    }

    class SettingTab
    {
        public string Name;
        public string[] SettingKeys;
    }

    class Setting
    {
        public string Key;
        public string Label;
        public string[] Values = Array.Empty<string>();
        public string[] Labels;
        public bool Text;
        public bool Slider;
        public string Action;
        public bool IsResolution = false; // when true, value is "WxH" from Values
    }


public sealed partial class ClassicSurface
{
void InitializeSettings() {
            _settingsTabs = new[]
            {
                new SettingTab { Name = "Display", SettingKeys = new[] { "window", "window_size", "pacing", "preload_shaders", "ui_language" } },
                new SettingTab { Name = "Graphics", SettingKeys = new[] { "render_scale", "gpu", "renderer", "overlay", "hide_cursor" } },
                new SettingTab { Name = "Audio", SettingKeys = new[] { "volume" } },
                new SettingTab { Name = "Online", SettingKeys = new[] { "name", "lobby", "code", "server", "direct" } },
                new SettingTab { Name = "Controllers", SettingKeys = new[] { "keyboard_player", "background", "pads", "remap" } },
                new SettingTab { Name = "Update", SettingKeys = Array.Empty<string>() },
                new SettingTab { Name = "Storage", SettingKeys = Array.Empty<string>() },
                new SettingTab { Name = "Game", SettingKeys = new[] { "game_files" } },
                new SettingTab { Name = "Theme", SettingKeys = new[] { "theme_preset", "theme_accent", "theme_background", "theme_text", "theme_save", "theme_reset" } },
            };
            if (RoomsSupported) _settingsTabs = [.. _settingsTabs, new SettingTab { Name = "Advanced", SettingKeys = Array.Empty<string>() }];   // rooms: RoomsArtwork.cs

            _settings = new[]
            {
                new Setting { Key = "theme_preset", Label = "Color preset", Values = new[] { "custom" }, Labels = new[] { "Custom colors" } },
                new Setting { Key = "theme_save", Label = "Save your colors", Values = new[] { "Save as preset" } },
                new Setting { Key = "theme_accent", Label = "Accent color", Text = true },
                new Setting { Key = "theme_background", Label = "Background color", Text = true },
                new Setting { Key = "theme_text", Label = "Text color", Text = true },
                new Setting { Key = "theme_reset", Label = "Default colors", Action = "theme:reset" },
                new Setting { Key = "ui_language", Label = "Interface language", Values = UiLocale.Codes, Labels = UiLocale.Names },
                new Setting { Key = "window", Label = "Window", Values = SettingDefaults.Choices["window"], Labels = new[] { "A window", "Borderless (whole monitor)", "Full screen" } },
                new Setting { Key = "window_size", Label = "Window size", Values = SettingDefaults.Choices["window_size"] },
                new Setting { Key = "pacing", Label = "Frame pacing", Values = SettingDefaults.Choices["pacing"], Labels = new[] { "Steady 60 (any monitor)", "G-Sync / FreeSync monitor", "Follow the monitor's refresh" } },
                new Setting { Key = "preload_shaders", Label = "Preload all shaders", Action = "settings:shaders" },
                new Setting { Key = "game_files", Label = "Add DLC or change game files", Action = "settings:game" },
                new Setting { Key = "render_scale", Label = "Render scale", Values = SettingDefaults.Choices["render_scale"], Labels = new[] { "1: 1920x1080 (the game's own)", "Auto: as big as the window", "1.5: 2880x1620", "2: 3840x2160 (4K)", "3: 5760x3240" } },
                new Setting { Key = "gpu", Label = "Graphics card", Values = new[] { "auto", "warp" }, Labels = new[] { "The fastest (auto)", "WARP (Windows itself, no card: slow)" } },
                new Setting { Key = "renderer", Label = "Renderer", Values = SettingDefaults.Choices["renderer"], Labels = new[] { "Auto: Vulkan on AMD, NVIDIA and Intel", "Direct3D 11", "Vulkan" } },
                new Setting { Key = "hide_cursor", Label = "Hide the mouse pointer", Values = new[] { "1", "0" }, Labels = new[] { "When it stops moving", "Keep it shown" } },
                new Setting { Key = "overlay", Label = "Frame rate", Values = new[] { "1", "0" }, Labels = new[] { "Show it in the corner", "Hidden" } },
                new Setting { Key = "volume", Label = "Volume", Slider = true },
                new Setting { Key = "name", Label = "Your name", Text = true },
                new Setting { Key = "lobby", Label = "Lobby", Values = SettingDefaults.Choices["lobby"], Labels = new[] { "Public lobby (Europe)", "Private lobby (a code)", "Your own server", "This network (LAN)", "Offline" } },
                new Setting { Key = "code", Label = "Lobby code", Text = true },
                new Setting { Key = "server", Label = "Server", Text = true },
                new Setting { Key = "direct", Label = "Direct connections", Values = new[] { "on", "off" }, Labels = new[] { "On", "Off" } },
                new Setting { Key = "keyboard_player", Label = "Keyboard plays as", Values = new[] { "1", "2", "3", "4", "5", "6", "7", "8", "0" }, Labels = new[] { "Player 1", "Player 2", "Player 3", "Player 4", "Player 5", "Player 6", "Player 7", "Player 8", "Nobody (no keyboard)" } },
                new Setting { Key = "background", Label = "Controllers in the background", Values = new[] { "1", "0" }, Labels = new[] { "Work while another window is in front", "Only while the game is in front" } },
                new Setting { Key = "remap", Label = "Remap a controller", Action = "settings:remap" },
                new Setting { Key = "pads", Label = "controllers.ini", Values = new[] { "open" }, Labels = new[] { "Open the file" } },
            };
}
        public Func<bool> ShadersPending {get;set;}=()=>false;
        bool SettingVisible(string key)=>key!="preload_shaders"||_shaderUi||ShadersPending();

        bool SettingOn(Setting s)
        {
            if (s.Key == "preload_shaders") return Project?.IsBuilt == true;
            if (s.Key == "game_files") return Project?.IsBuilt == true && !Busy;
            string lobby = _iniValues.GetValueOrDefault("lobby", "offline");
            if (s.Key is "name" or "direct") return lobby != "offline";
            if (s.Key == "code") return lobby is "private" or "custom";
            if (s.Key == "server") return lobby == "custom";
            return true;
        }

        string ShownValue(Setting s)
        {
            if (s.Key == "preload_shaders") return _shaderUi ? "Cancel compilation" : "Compile shaders";
            if (s.Key == "game_files") return "Open setup";
            if (s.Key == "theme_reset") return "Reset colors";
            if (s.Key == "remap") return "Press a button for each of the game's";
            string v = _iniValues.GetValueOrDefault(s.Key, s.Text || s.Slider || s.Values.Length == 0 ? "" : s.Values[0]);
            if (s.Slider) return v;
            if (s.Text) return v == "" ? "type here" : v;
            if (s.Labels != null)
            {
                int i = Array.IndexOf(s.Values, v);
                if (i >= 0 && i < s.Labels.Length) return s.Labels[i];
            }
            return v;
        }

        int SectionStart(int tab)
        {
            int n = 0;
            for (int i = 0; i < tab && i < _settingsTabs.Length; i++)
                n += _settingsTabs[i].SettingKeys.Count(SettingVisible);
            return n;
        }

        Setting[] GetVisibleSettings()
        {
            var list = new List<Setting>();
            foreach (var tab in _settingsTabs)
                foreach (var key in tab.SettingKeys)
                {
                    var set = _settings.FirstOrDefault(s => s.Key == key);
                    if (set != null && SettingVisible(key)) list.Add(set);
                }
            return list.ToArray();
        }

        static string IniValue(string path, string section, string key)
        {
            if (!File.Exists(path)) return null;
            bool inSection = section == "";
            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                if (line[0] == '[')
                {
                    int end = line.IndexOf(']');
                    string name = end > 1 ? line.Substring(1, end - 1).Trim() : "";
                    inSection = section != "" && name.Equals(section, StringComparison.OrdinalIgnoreCase);
                    continue;
                }
                if (!inSection) continue;
                int eq = line.IndexOf('=');
                if (eq < 1) continue;
                if (!line.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) continue;
                string v = line.Substring(eq + 1).Trim();
                int cut = v.Length;
                for (int i = 1; i < v.Length; i++)
                    if ((v[i] == ';' || v[i] == '#') && (v[i - 1] == ' ' || v[i - 1] == '\t')) { cut = i; break; }
                return v.Substring(0, cut).Trim();
            }
            return null;
        }

        static string FirstLine(string path)
        {
            if (!File.Exists(path)) return "";
            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Trim();
                if (line.Length > 0 && line[0] != '#') return line;
            }
            return "";
        }

        static void SetIni(string path, string section, string key, string value)
        {
            var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
            bool inSection = section == "";
            int sectionAt = section == "" ? 0 : -1;
            int keyAt = -1;
            for (int i = 0; i < lines.Count; i++)
            {
                string line = lines[i].Trim();
                if (line.StartsWith("["))
                {
                    int end = line.IndexOf(']');
                    string name = end > 1 ? line.Substring(1, end - 1).Trim() : "";
                    inSection = section != "" && name.Equals(section, StringComparison.OrdinalIgnoreCase);
                    if (inSection) sectionAt = i;
                    continue;
                }
                if (!inSection || line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int eq = line.IndexOf('=');
                if (eq > 0 && line.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) { keyAt = i; break; }
            }
            string row = key + " = " + value;
            if (keyAt >= 0) lines[keyAt] = row;
            else if (section == "")
            {
                int insertAt = lines.Count;
                for (int i = 0; i < lines.Count; i++)
                    if (lines[i].TrimStart().StartsWith("[")) { insertAt = i; break; }
                lines.Insert(insertAt, row);
            }
            else if (sectionAt >= 0 && sectionAt < lines.Count) lines.Insert(sectionAt + 1, row);
            else
            {
                if (lines.Count > 0 && lines[lines.Count - 1].Trim() != "") lines.Add("");
                if (section != "") lines.Add("[" + section + "]");
                lines.Add(row);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllLines(path, lines, new UTF8Encoding(false));
        }

        static void SetFirstLine(string path, string value)
        {
            var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
            int at = -1;
            for (int i = 0; i < lines.Count; i++)
            {
                string t = lines[i].Trim();
                if (t.Length > 0 && t[0] != '#') { at = i; break; }
            }
            if (string.IsNullOrEmpty(value)) { if (at >= 0) lines.RemoveAt(at); }
            else if (at >= 0) lines[at] = value;
            else lines.Add(value);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllLines(path, lines, new UTF8Encoding(false));
        }

        void ReadSettings()
        {
            _iniValues.Clear();_changedSettings.Clear();
            string display = Project.ConfigFile("display.ini");
            string pads = Project.ConfigFile("controllers.ini");
            // A value the files don't have is the game's own default (SettingDefaults.cs, checked against the runtime).
            string D(string key) => IniValue(display, "", key) ?? SettingDefaults.Of(key);
            _iniValues["ui_language"] = D("ui_language");
            UiLocale.Select(_iniValues["ui_language"]);
            foreach (string key in new[] { "window", "width", "height", "pacing", "render_scale", "gpu", "hide_cursor", "overlay", "volume" })
                _iniValues[key] = D(key);
            _iniValues["window_size"] = _iniValues["width"] + "x" + _iniValues["height"];
            _iniValues["renderer"] = SettingDefaults.Renderer(D("renderer"));
            foreach (string key in new[] { "keyboard_player", "background" })
                _iniValues[key] = IniValue(pads, "general", key) ?? SettingDefaults.Of(key);
            // online_server.txt is the truth (the game's settings menu writes it too); launcher.ini only remembers the
            // code and the server typed here, for when that choice is picked again.
            string serverFile = Project.ConfigFile("online_server.txt");
            string line = File.Exists(serverFile) ? FirstLine(serverFile) : SettingDefaults.OfFile("online_server.txt");
            var (lobby, code, server) = SettingDefaults.LobbyFromLine(line);
            string launcherIni = Project.ConfigFile("launcher.ini");
            _iniValues["lobby"] = lobby;
            _iniValues["code"] = code != "" ? code : IniValue(launcherIni, "opensmash", "code") ?? "";
            string savedServer = IniValue(launcherIni, "opensmash", "server") ?? "";
            if (SettingDefaults.MeansPublicLobby(savedServer)) savedServer = "";   // (issue #389: an old public lobby, not "your own server")
            _iniValues["server"] = server != "" ? server : savedServer;
            // (the gone North American lobby's address still in online_server.txt: written as what it means, so no launcher puts it back; issue #389)
            if (File.Exists(serverFile) && SettingDefaults.IsOldLobbyLine(line))
                try { SetFirstLine(serverFile, SettingDefaults.LineFromLobby(lobby, code, server)); } catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            string directFile = Project.ConfigFile("online_direct.txt");
            string direct = File.Exists(directFile) ? FirstLine(directFile) : SettingDefaults.OfFile("online_direct.txt");
            _iniValues["direct"] = direct.StartsWith("off", StringComparison.OrdinalIgnoreCase) ? "off" : "on";
            _iniValues["name"] = FirstLine(Project.ConfigFile("online_name.txt"));
            var gpu = _settings.First(s => s.Key == "gpu");
            if (!gpu.Values.Contains(_iniValues["gpu"]))
            {
                gpu.Values = gpu.Values.Append(_iniValues["gpu"]).ToArray();
                gpu.Labels = gpu.Labels.Append(_iniValues["gpu"]).ToArray();
            }
            foreach (var (key, fallback) in ThemeDefaults())
            {
                string value = IniValue(launcherIni, "theme", key) ?? fallback;
                _iniValues[key] = ValidThemeColor(value) ? value.ToUpperInvariant() : fallback;
            }
            ApplyTheme();
            LoadThemePresets();
            _settingsLoaded = true;
            var size = _settings.First(s => s.Key == "window_size");
            if (!size.Values.Contains(_iniValues["window_size"]))
                size.Values = new[] { _iniValues["window_size"] }.Concat(size.Values).ToArray();
            var scale = _settings.First(s => s.Key == "render_scale");
            if (!scale.Values.Contains(_iniValues["render_scale"]))
            {
                scale.Values = scale.Values.Append(_iniValues["render_scale"]).ToArray();
                scale.Labels = scale.Labels.Append(_iniValues["render_scale"]).ToArray();
            }
        }

        void WriteSettingsFiles()
        {
            string lobby = _iniValues.GetValueOrDefault("lobby", "offline");
            string code = _iniValues.GetValueOrDefault("code", "").Replace(" ", "");
            string host = _iniValues.GetValueOrDefault("server", "").Trim();
            bool serverReady = lobby != "private" || code != "";
            if (lobby == "custom" && host != "" && !System.Text.RegularExpressions.Regex.IsMatch(host, @"^\S+:\d+$"))
                serverReady = false;
            // Only what was changed here since the files were read: the game's own settings menu (Esc) writes these
            // files too, while the launcher waits for the game to close.
            string display = Project.ConfigFile("display.ini"), pads = Project.ConfigFile("controllers.ini");
            foreach (string key in new[] { "window", "width", "height", "pacing", "render_scale", "gpu", "renderer", "hide_cursor", "overlay", "volume" })
                if (_changedSettings.Contains(key)) SetIni(display, "", key, _iniValues.GetValueOrDefault(key, SettingDefaults.Of(key)));
            foreach (string key in new[] { "keyboard_player", "background" })
                if (_changedSettings.Contains(key)) SetIni(pads, "general", key, _iniValues.GetValueOrDefault(key, SettingDefaults.Of(key)));
            bool serverChanged = _changedSettings.Overlaps(["lobby", "code", "server"]);
            if (serverReady && serverChanged) SetFirstLine(Project.ConfigFile("online_server.txt"), SettingDefaults.LineFromLobby(lobby, code, host));
            if (_changedSettings.Contains("name")) SetFirstLine(Project.ConfigFile("online_name.txt"), _iniValues.GetValueOrDefault("name", "").Trim());
            if (_changedSettings.Contains("direct")) SetFirstLine(Project.ConfigFile("online_direct.txt"), _iniValues.GetValueOrDefault("direct", SettingDefaults.OfFile("online_direct.txt")));
            string launcherIni = Project.ConfigFile("launcher.ini");
            _changedSettings.Clear();   // (written: the game's later changes stay)
            if (serverChanged && !serverReady) _changedSettings.UnionWith(["lobby", "code", "server"]);
            // (only when changed here: a stale copy once hid the lobby the game's menu had picked)
            if (serverChanged)
            {
                if (code != "") SetIni(launcherIni, "opensmash", "code", code);
                if (host != "") SetIni(launcherIni, "opensmash", "server", host);
            }
        }
}
