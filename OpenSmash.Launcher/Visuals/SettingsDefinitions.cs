// Original Windows launcher definitions, now maintained in the shared implementation.
#nullable disable
#pragma warning disable CS0649 // Original view-model fields include optional mod variants.
using System.Text;
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
                new SettingTab { Name = "Display", SettingKeys = new[] { "window", "window_size", "pacing", "preload_shaders" } },
                new SettingTab { Name = "Graphics", SettingKeys = new[] { "render_scale", "gpu", "renderer", "overlay", "hide_cursor" } },
                new SettingTab { Name = "Audio", SettingKeys = new[] { "volume" } },
                new SettingTab { Name = "Online", SettingKeys = new[] { "name", "lobby", "code", "server", "direct" } },
                new SettingTab { Name = "Controllers", SettingKeys = new[] { "keyboard_player", "background", "pads" } },
                new SettingTab { Name = "Update", SettingKeys = Array.Empty<string>() },
                new SettingTab { Name = "Storage", SettingKeys = Array.Empty<string>() },
                new SettingTab { Name = "Developer", SettingKeys = new[] { "hot_reload_mod_assets" } },
            };

            _settings = new[]
            {
                new Setting { Key = "window", Label = "Window", Values = new[] { "windowed", "borderless", "fullscreen" }, Labels = new[] { "A window", "Borderless (whole monitor)", "Full screen" } },
                new Setting { Key = "window_size", Label = "Window size", Values = new[] { "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" } },
                new Setting { Key = "pacing", Label = "Frame pacing", Values = new[] { "clock", "vrr", "display" }, Labels = new[] { "Steady 60 (any monitor)", "G-Sync / FreeSync monitor", "Follow the monitor's refresh" } },
                new Setting { Key = "preload_shaders", Label = "Preload all shaders", Action = "settings:shaders" },
                new Setting { Key = "render_scale", Label = "Render scale", Values = new[] { "1", "auto", "1.5", "2", "3" }, Labels = new[] { "1: 1920x1080 (the game's own)", "Auto: as big as the window", "1.5: 2880x1620", "2: 3840x2160 (4K)", "3: 5760x3240" } },
                new Setting { Key = "gpu", Label = "Graphics card", Values = new[] { "auto", "warp" }, Labels = new[] { "The fastest (auto)", "WARP (Windows itself, no card: slow)" } },
                new Setting { Key = "renderer", Label = "Renderer", Values = new[] { "auto", "direct3d11", "vulkan" }, Labels = new[] { "Auto: Vulkan on AMD, NVIDIA and Intel", "Direct3D 11", "Vulkan" } },
                new Setting { Key = "hide_cursor", Label = "Hide the mouse pointer", Values = new[] { "1", "0" }, Labels = new[] { "When it stops moving", "Keep it shown" } },
                new Setting { Key = "overlay", Label = "Frame rate", Values = new[] { "1", "0" }, Labels = new[] { "Show it in the corner", "Hidden" } },
                new Setting { Key = "volume", Label = "Volume", Slider = true },
                new Setting { Key = "name", Label = "Your name", Text = true },
                new Setting { Key = "lobby", Label = "Lobby", Values = new[] { "public", "private", "custom", "offline" }, Labels = new[] { "Public lobby", "Private lobby (a code)", "Your own server", "Offline" } },
                new Setting { Key = "code", Label = "Lobby code", Text = true },
                new Setting { Key = "server", Label = "Server", Text = true },
                new Setting { Key = "direct", Label = "Direct connections", Values = new[] { "on", "off" }, Labels = new[] { "On", "Off" } },
                new Setting { Key = "keyboard_player", Label = "Keyboard plays as", Values = new[] { "1", "2", "3", "4", "5", "6", "7", "8", "0" }, Labels = new[] { "Player 1", "Player 2", "Player 3", "Player 4", "Player 5", "Player 6", "Player 7", "Player 8", "Nobody (no keyboard)" } },
                new Setting { Key = "background", Label = "Controllers in the background", Values = new[] { "1", "0" }, Labels = new[] { "Work while another window is in front", "Only while the game is in front" } },
                new Setting { Key = "pads", Label = "controllers.ini", Values = new[] { "open" }, Labels = new[] { "Open the file" } },
                new Setting { Key = "hot_reload_mod_assets", Label = "Hot reload mod assets in training mode by doing L + R + A", Values = new[] { "0", "1" }, Labels = new[] { "Disabled", "Enabled" } },
            };
}
        public Func<bool> ShadersPending {get;set;}=()=>false;
        bool SettingVisible(string key)=>key!="preload_shaders"||_shaderUi||ShadersPending();

        bool SettingOn(Setting s)
        {
            if (s.Key == "preload_shaders") return Project?.IsBuilt == true;
            string lobby = _iniValues.GetValueOrDefault("lobby", "public");
            if (s.Key is "name" or "direct") return lobby != "offline";
            return true;
        }

        string ShownValue(Setting s)
        {
            if (s.Key == "preload_shaders") return _shaderUi ? "Cancel compilation" : "Compile shaders";
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
            string D(string key, string fallback) => IniValue(display, "", key) ?? fallback;
            _iniValues["window"] = D("window", "borderless");
            _iniValues["width"] = D("width", "1920");
            _iniValues["height"] = D("height", "1080");
            _iniValues["window_size"] = _iniValues["width"] + "x" + _iniValues["height"];
            _iniValues["pacing"] = D("pacing", "display");
            _iniValues["render_scale"] = D("render_scale", "1");
            _iniValues["gpu"] = D("gpu", "auto");
            // (runtime/renderer.h's names: d3d11, which older display.ini files carry, is auto)
            _iniValues["renderer"] = D("renderer", OperatingSystem.IsWindows()?"vulkan":"auto").Trim().ToLowerInvariant() switch { "vulkan" or "vk" => "vulkan", "direct3d11" or "direct3d" or "dx11" => "direct3d11", _ => "auto" };
            _iniValues["hide_cursor"] = D("hide_cursor", "1");
            _iniValues["overlay"] = D("overlay", "1");
            _iniValues["volume"] = D("volume", "100");
            _iniValues["keyboard_player"] = IniValue(pads, "general", "keyboard_player") ?? "1";
            _iniValues["background"] = IniValue(pads, "general", "background") ?? "1";
            string server = FirstLine(Project.ConfigFile("online_server.txt"));
            string[] words = server.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            string host = words.Length > 0 ? words[0] : "";
            _iniValues["code"] = words.Length > 1 ? words[1] : "";
            const string Public = "smash.recomps.wtf:39421";
            if (host == "" || host.Equals("off", StringComparison.OrdinalIgnoreCase)) _iniValues["lobby"] = "offline";
            else if (host.Equals("auto", StringComparison.OrdinalIgnoreCase) || host.Equals("na", StringComparison.OrdinalIgnoreCase) || host.Equals("eu", StringComparison.OrdinalIgnoreCase)) _iniValues["lobby"] = "public";   // (auto: the nearest public lobby, the default)
            else if (host.Equals(Public, StringComparison.OrdinalIgnoreCase)) _iniValues["lobby"] = _iniValues["code"] == "" ? "public" : "private";
            else { _iniValues["lobby"] = "custom"; _iniValues["server"] = host; }
            if (!_iniValues.ContainsKey("server")) _iniValues["server"] = "";
            string launcherIni = Project.ConfigFile("launcher.ini");
            _iniValues["hot_reload_mod_assets"] = IniValue(launcherIni, "opensmash", "hot_reload_mod_assets") == "1" ? "1" : "0";
            string savedLobby = IniValue(launcherIni, "opensmash", "lobby");
            string savedCode = IniValue(launcherIni, "opensmash", "code");
            string savedServer = IniValue(launcherIni, "opensmash", "server");
            bool preferSavedLobby = false;
            if (File.Exists(launcherIni))
            {
                DateTime savedAt = File.GetLastWriteTimeUtc(launcherIni);
                string serverFile = Project.ConfigFile("online_server.txt");
                DateTime serverAt = File.Exists(serverFile) ? File.GetLastWriteTimeUtc(serverFile) : DateTime.MinValue;
                preferSavedLobby = savedAt >= serverAt;
            }
            if (preferSavedLobby && savedLobby is "public" or "private" or "custom" or "offline")
                _iniValues["lobby"] = savedLobby;
            if (string.IsNullOrEmpty(_iniValues["code"]) && savedCode != null)
                _iniValues["code"] = savedCode;
            if ((preferSavedLobby || string.IsNullOrEmpty(_iniValues["server"])) && savedServer != null)
                _iniValues["server"] = savedServer;
            string direct = FirstLine(Project.ConfigFile("online_direct.txt"));
            _iniValues["direct"] = direct.Equals("off", StringComparison.OrdinalIgnoreCase) ? "off" : "on";
            _iniValues["name"] = FirstLine(Project.ConfigFile("online_name.txt"));
            var gpu = _settings.First(s => s.Key == "gpu");
            if (!gpu.Values.Contains(_iniValues["gpu"]))
            {
                gpu.Values = gpu.Values.Append(_iniValues["gpu"]).ToArray();
                gpu.Labels = gpu.Labels.Append(_iniValues["gpu"]).ToArray();
            }
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
            string lobby = _iniValues.GetValueOrDefault("lobby", "public");
            string code = _iniValues.GetValueOrDefault("code", "").Replace(" ", "");
            string host = _iniValues.GetValueOrDefault("server", "").Trim();
            bool serverReady = lobby != "private" || code != "";
            if (lobby == "custom" && host != "" && !System.Text.RegularExpressions.Regex.IsMatch(host, @"^\S+:\d+$"))
                serverReady = false;
            const string Public = "smash.recomps.wtf:39421";
            string server = lobby switch
            {
                "offline" => "off",
                "private" => Public + " " + code,
                "custom" => (host == "" ? "127.0.0.1:39420" : host) + (code == "" ? "" : " " + code),
                _ => "auto"   // (the nearest public lobby: Europe or North America, runtime/net_relay.c)
            };
            foreach(var (key,fallback) in new[]{("window","borderless"),("width","1920"),("height","1080"),("pacing","display"),("render_scale","1"),("gpu","auto"),("renderer",OperatingSystem.IsWindows()?"vulkan":"auto"),("hide_cursor","1"),("overlay","1"),("volume","100")})
                if(_changedSettings.Contains(key))SetIni(Project.ConfigFile("display.ini"),"",key,_iniValues.GetValueOrDefault(key,fallback));
            foreach(var (key,fallback) in new[]{("keyboard_player","1"),("background","1")})
                if(_changedSettings.Contains(key))SetIni(Project.ConfigFile("controllers.ini"),"general",key,_iniValues.GetValueOrDefault(key,fallback));
            bool serverChanged=_changedSettings.Overlaps(["lobby","code","server"]);
            if(serverReady&&serverChanged)SetFirstLine(Project.ConfigFile("online_server.txt"),server);
            if(_changedSettings.Contains("name"))SetFirstLine(Project.ConfigFile("online_name.txt"),_iniValues.GetValueOrDefault("name","").Trim());
            if(_changedSettings.Contains("direct"))SetFirstLine(Project.ConfigFile("online_direct.txt"),_iniValues.GetValueOrDefault("direct","on"));
            if(_changedSettings.Contains("hot_reload_mod_assets"))SetIni(Project.ConfigFile("launcher.ini"),"opensmash","hot_reload_mod_assets",_iniValues.GetValueOrDefault("hot_reload_mod_assets","0"));
            _changedSettings.Clear();
            if(serverChanged&&!serverReady)_changedSettings.UnionWith(["lobby","code","server"]);
            string launcherIni = Project.ConfigFile("launcher.ini");
            SetIni(launcherIni, "opensmash", "lobby", lobby);
            SetIni(launcherIni, "opensmash", "code", code);
            SetIni(launcherIni, "opensmash", "server", host);
        }
}
