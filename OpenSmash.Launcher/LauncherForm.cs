using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Web.WebView2.WinForms;
using Microsoft.Web.WebView2.Core;

namespace OpenSmash.Launcher
{
    // ============================================================================
    // LauncherForm: Dusklight-style launcher with installer flow
    // ============================================================================
    public partial class LauncherForm : Form
    {
        // ---------------------------------------------------------------------------
        // Colours (shared with the installer, see Theme)
        static readonly Color C_BG      = Theme.Bg;
        static readonly Color C_TEXT    = Theme.Text;
        static readonly Color C_MUTED   = Theme.Muted;
        static readonly Color C_DIM     = Theme.Dim;
        static readonly Color C_ACCENT  = Theme.Accent;
        static readonly Color C_ACCENT2 = Theme.Accent2;
        static readonly Color C_READY   = Theme.Ready;
        static readonly Color C_BAD     = Theme.Bad;
        static readonly Color C_LINE    = Theme.Line;
        static readonly Color C_OK      = Theme.Ready;

        // Shared HttpClient for all network requests
        static readonly HttpClient _httpClient = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
        static LauncherForm() { _httpClient.DefaultRequestHeaders.Add("User-Agent", "OpenSmash-Launcher/1.0"); }

        const string UpdateRepo = "OpenSmash-Ultimate/OpenSmash-Ultimate";
        const string UpdateApi = "https://api.github.com/repos/" + UpdateRepo;

        // ---------------------------------------------------------------------------
        // Paths & state
        readonly string _workDir;
        readonly string _repo;
        readonly string _buildDir;
        readonly string _modsDir;
        readonly string _skylineDir;
        readonly string _pluginsDir;
        readonly string _iniPath;
        readonly string _statePath;
        readonly string _pluginsPath;
        string _pythonExe;

        readonly ResolutionSetting _resolutionSetting = new ResolutionSetting();

        string _version = "";
        bool _built = false;
        int _plugins = 0;
        string _rendererIni = "";   // display.ini's renderer, as RefreshInfo last read it
        List<ModInfo> _mods = new();
        bool _modsModded;
        bool _modsChecked;
        bool _modsBusy;
        bool _startingPlay;
        string _pluginBuildReason = "";
        string _vanillaBuildReason = "";   // (os_mods status: needs_build_vanilla, the link Play without mods needs)
        readonly Dictionary<string, Image> _modThumbs = new();
        readonly HashSet<string> _openGroups = new();
        readonly HashSet<string> _knownGroups = new();
        List<ModLine> _modLines = new();
        List<(string Name, string Version, bool Fetched, string Status, string Source)> _stdPlugins = new();
        string _view = "home"; // home, mods, mods_browser, setup_modding, settings
        readonly Stack<string> _navBack = new();
        int _sel = 0;
        string _hover = "";
        string _pressed = "";
        bool _keys = false;
        int _modScroll = 0;
        Dictionary<string, string> _iniValues = new();
        readonly HashSet<string> _changed = new();   // settings changed here since ReadSettings: the only ones written

        // WebView2 for MOSAIC marketplace
        WebView2 _marketplaceWebView;
        bool _marketplaceInitialized = false;
        bool _marketplaceDetail = false;
        string _marketplaceError = "";
        
        // Modding setup
        bool _setupModding = false;
        int _setupProgress = 0;
        string _setupStep = "";
        string _setupLog = "";
        string _setupMode = ""; // "scratch" or "import"

        // Play (mods) or Vanilla (the game's own data and no mods: mods.play_vanilla); both start build\ssbu.exe. Remembered in build\launch_mode.txt.
        string _launchMode = "mods";
        Task<bool> _prepInflight;   /* mod convert + hook install, shared by launcher startup and Play */
        bool _playQueued;

        // A mod dropped straight into mods\ while OpenSmash was closed: it is not in mods.json yet,
        // so its plugin has not been built. The boot prompt offers to do that.
        bool _newModsPrompt;
        bool _newModsPending;
        bool _newModsBusy;
        readonly List<string> _newMods = new();

        // A newer commit on GitHub's main. Checked when the window opens and from the Update tab.
        bool _updateChecking, _updateApplying, _updatePromptPending, _updatePrompt, _updateDontShow;
        string _updateState = "idle";   // idle, checking, uptodate, available, error, updating
        string _updateMessage = "";
        string _updateDetail = "";
        string _updateLocal = "";
        string _updateHead = "";
        int _updateBehind;

        // While any of these run, Play is greyed out so two jobs cannot overlap.
        bool Busy => _modsBusy || _setupModding || _pluginBuildUi || _loadOn || _startingPlay || _newModsBusy
            || _updateApplying || _marketplaceInstalling.Count > 0;

        bool _rebuildPrompt;
        bool _sharePrompt;
        bool _checkModsPrompt;
        string _checkModsText = "";
        List<(string gb, string file, string title)> _checkModsUpdates = new();

        // Game overlay state
        Process _gameProcess;
        bool _loadOn;
        string _loadLabel = "";
        float _loadProgress = -1;          // 0..1, or -1 while the length is not known yet
        bool _pluginBuildUi;
        float _pluginFrac = -1;            // 0..1, or -1 before the build has a length
        string _pluginDetail = "";
        string _pluginEta = "";
        bool _shaderPrompt;
        double _shaderPromptAt;
        bool _shaderUi;
        float _shaderFrac = -1;
        string _shaderDetail = "";
        string _shaderEta = "";
        Process? _shaderProc;
        bool _gameLaunched = false;
        bool _gameReady = false;
        bool _waitingForGame = false;

        // Settings dropdown state
        string _openDropdownKey = null;
        int _dropdownHoverIndex = -1;
        int _lastClickX = 0;
        float _volTrackX, _volTrackW, _volTrackY, _volTrackH;
        bool _volTrackValid;
        bool _volDragging;
        TextBox _settingEdit;
        string _editingKey;
        bool _settingsLoaded;
        readonly Dictionary<string, Rectangle> _editRects = new();

        // No-mods setup prompt, and the same card for a plugin that still needs building.
        bool _showSetupPrompt = false;
        bool _pluginPrompt;
        string _pluginPromptKind = "update";   // "update": link the game. "refresh": rebuild a plugin DLL.
        string _pluginPromptText = "";
        string _pluginRefreshReason = "";
        TaskCompletionSource<string> _pluginPromptWait;

        // Emulator import state
        List<EmulatorInfo> _foundEmulators = new();
        int _importSel = 0;

        // Fonts
        Font _fDisplay, _fMenu, _fUI, _fSmall, _fMono;
        Image _logo;

        // Hit-testing
        List<Hit> _hits = new();

        // ---------------------------------------------------------------------------
        public LauncherForm(string workDir, string repo, string buildDir)
        {
            _workDir = workDir;
            _repo = repo;
            _buildDir = buildDir;
            _modsDir = Path.Combine(repo, "mods");
            LoadInstantMods();
            _skylineDir = Path.Combine(repo, "sd", "atmosphere", "contents", "01006A800016E000");
            _pluginsDir = Path.Combine(repo, "skyline", "plugins");
        // Migrate legacy subsdk9 location (sd/ultimate/skyline/exefs) to the
        // layered-fs exefs folder the game actually loads (sd/atmosphere/.../exefs).
        try
        {
            string legacy = Path.Combine(repo, "sd", "ultimate", "skyline", "exefs", "subsdk9");
            string target = Path.Combine(_skylineDir, "exefs", "subsdk9");
            if (File.Exists(legacy) && !File.Exists(target))
            {
                Directory.CreateDirectory(Path.Combine(_skylineDir, "exefs"));
                File.Copy(legacy, target, true);
            }
        }
        catch { }
            _iniPath = Path.Combine(buildDir, "config", "graphics.ini");
            _statePath = Path.Combine(buildDir, "state.json");
            _pluginsPath = Path.Combine(buildDir, "work", "plugin_bin", "plugins.txt");

            _pythonExe = ModManagerProcess.FindPython(repo, buildDir);

            // Window setup
            Text = "OpenSmash-Ultimate";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = C_BG;
            DoubleBuffered = true;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            MinimizeBox = true;
            MaximizeBox = false;
            ShowInTaskbar = true;

            // Load icon
            try
            {
                string icoPath = Path.Combine(workDir, "opensmash.ico");
                if (File.Exists(icoPath)) Icon = new Icon(icoPath);
            }
            catch { }

            ClientSize = Theme.WindowSize();
            // Moved to a monitor with other scaling: take the size Windows suggests; the drawing follows the height.
            DpiChanged += (s, e) => { Bounds = e.SuggestedRectangle; Invalidate(); };
            // Borderless forms sometimes skip the taskbar until clicked; APPWINDOW forces the button.
            // (CreateParams override below.)

            // Load logo
            try
            {
                string logoPath = Path.Combine(workDir, "emblem.png");
                if (File.Exists(logoPath)) _logo = Image.FromFile(logoPath);
            }
            catch { }

            // Fonts
            _fDisplay = NewFont("Bahnschrift SemiBold Condensed", 1);
            _fMenu = NewFont("Bahnschrift SemiBold Condensed", 1);
            _fUI = NewFont("Segoe UI", 1);
            _fSmall = NewFont("Segoe UI", 1);
            _fMono = NewFont("Consolas", 1);

            // Events
            Paint += OnPaint;
            MouseMove += OnMouseMove;
            MouseLeave += OnMouseLeave;
            MouseDown += OnMouseDown;
            MouseUp += OnMouseUp;
            MouseWheel += OnMouseWheel;
            KeyDown += OnKeyDown;
            FormClosing += (s, e) => 
            { 
                CommitEdit();
                PersistSettings();
                // Only kill the game if the launcher is closing without having
                // launched it (Play sets _waitingForGame and lets it run on).
                if (!_waitingForGame && _gameProcess != null && !_gameProcess.HasExited)
                {
                    try { _gameProcess.Kill(); } catch { }
                }
                if (_shaderProc != null && !_shaderProc.HasExited)
                {
                    try { _shaderProc.Kill(entireProcessTree: true); } catch { }
                }
            };
            Shown += async (s, e) =>
            {
                AskShadersIfNeeded();
                _ = EnsureOwnPythonAsync();   // (not awaited: the launcher does not wait for pip, nor stop if it fails)
                UpdateMods();
                ScanForNewMods();
                _ = CheckUpdatesAtBootAsync();
                // The marketplace browser is heavy. Let the boot animation finish before it starts.
                await Task.Delay(1100);
                await InitializeMarketplaceWebView();
            };

            // Initial data
            RefreshInfo();
            LoadLaunchMode();

            _settingsTabs = new[]
            {
                new SettingTab { Name = "Display", SettingKeys = new[] { "window", "window_size", "pacing" } },
                new SettingTab { Name = "Graphics", SettingKeys = new[] { "render_scale", "gpu", "renderer", "overlay", "hide_cursor" } },
                new SettingTab { Name = "Audio", SettingKeys = new[] { "volume" } },
                new SettingTab { Name = "Online", SettingKeys = new[] { "name", "lobby", "code", "server", "direct" } },
                new SettingTab { Name = "Controllers", SettingKeys = new[] { "keyboard_player", "background", "pads" } },
                new SettingTab { Name = "Update", SettingKeys = Array.Empty<string>() },
                new SettingTab { Name = "Storage", SettingKeys = Array.Empty<string>() },
            };

            _settings = new[]
            {
                new Setting { Key = "window", Label = "Window", Values = new[] { "windowed", "borderless", "fullscreen" }, Labels = new[] { "A window", "Borderless (whole monitor)", "Full screen" } },
                new Setting { Key = "window_size", Label = "Window size", Values = new[] { "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" } },
                new Setting { Key = "pacing", Label = "Frame pacing", Values = new[] { "clock", "vrr", "display" }, Labels = new[] { "Steady 60 (any monitor)", "G-Sync / FreeSync monitor", "Follow the monitor's refresh" } },
                new Setting { Key = "render_scale", Label = "Render scale", Values = new[] { "1", "auto", "1.5", "2", "3" }, Labels = new[] { "1: 1920x1080 (the game's own)", "Auto: as big as the window", "1.5: 2880x1620", "2: 3840x2160 (4K)", "3: 5760x3240" } },
                new Setting { Key = "gpu", Label = "Graphics card", Values = new[] { "auto", "warp" }, Labels = new[] { "The fastest (auto)", "WARP (Windows itself, no card: slow)" } },
                new Setting { Key = "renderer", Label = "Renderer", Values = new[] { "auto", "direct3d11", "vulkan" }, Labels = new[] { "Auto: Vulkan on AMD, NVIDIA and Intel cards, Direct3D 11 on others", "Direct3D 11", "Vulkan" } },
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
            };
            _settingEdit = new TextBox
            {
                Visible = false,
                BorderStyle = BorderStyle.None,
                Multiline = true,
                WordWrap = false,
                AcceptsReturn = false,
                AutoSize = false,
                Font = new Font("Segoe UI", 15, FontStyle.Regular, GraphicsUnit.Pixel),
                BackColor = Color.FromArgb(18, 18, 20),
                ForeColor = Theme.Text,
                MaxLength = 64,
                Cursor = Cursors.IBeam,
            };
            _settingEdit.HandleCreated += (s, e) =>
                SendMessageStr(_settingEdit.Handle, 0x1501, (IntPtr)1, "type here");
            _settingEdit.Leave += (s, e) => CommitEdit();
            _settingEdit.KeyDown += (s, e) =>
            {
                if (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Escape) { CommitEdit(); e.SuppressKeyPress = true; }
            };
            Controls.Add(_settingEdit);
        }
        
        // ---------------------------------------------------------------------------
        // Font helper
        Font NewFont(string name, float px)
        {
            var families = FontFamily.Families.Select(f => f.Name).ToHashSet();
            if (!families.Contains(name)) name = "Segoe UI";
            return new Font(name, px, FontStyle.Regular, GraphicsUnit.Pixel);
        }

        Font FDisplay(float px) => Theme.Display(px);
        Font FBody(float px, bool bold = false) => Theme.Body(px, bold);

        // ---------------------------------------------------------------------------
        // Data refresh
        void RefreshInfo()
        {
            _version = WindowSmash.Version(_repo);
            _built = WindowSmash.IsBuilt(_repo);
            _plugins = 0;
            if (Directory.Exists(_pluginsDir))
                _plugins = Directory.GetFiles(_pluginsDir, "*.nro").Length;
            try { _rendererIni = IniValue(WindowSmash.ConfigFile(_repo, "display.ini"), "", "renderer") ?? ""; }
            catch { _rendererIni = ""; }
        }

        // The renderer the game starts with, as runtime/renderer.h reads it: SSBU_RENDERER, else
        // display.ini's renderer (vulkan or vk: Vulkan; direct3d11, d3d11, direct3d or dx11: Direct3D 11;
        // anything else: auto, Vulkan on an AMD, NVIDIA or Intel card and Direct3D 11 on others).
        string RendererAsked()
        {
            string v = Environment.GetEnvironmentVariable("SSBU_RENDERER") ?? "";
            if (v.Trim() == "") v = _settingsLoaded ? _iniValues.GetValueOrDefault("renderer", "") : _rendererIni;
            return v.Trim().ToLowerInvariant() switch { "vulkan" or "vk" => "vulkan", "direct3d11" or "d3d11" or "direct3d" or "dx11" => "direct3d11", _ => "auto" };
        }

        string RendererChip() => RendererAsked() switch { "vulkan" => "VULKAN RENDERER", "direct3d11" => "D3D11 RENDERER", _ => "AUTO RENDERER" };

        int _modsGen;

        void UpdateMods() { _ = RefreshMods(); }

        Task RefreshMods()
        {
            var done = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            int gen = System.Threading.Interlocked.Increment(ref _modsGen);
            Task.Run(() =>
            {
                int code = 1;
                string output = "";
                try { code = RunPython(new[] { Path.Combine(_repo, "tools", "os_mods.py"), "status" }, out output); }
                catch { done.TrySetResult(false); return; }
                try
                {
                    BeginInvoke(() =>
                    {
                        if (gen == _modsGen && !IsDisposed) ApplyModStatus(code, output);
                        done.TrySetResult(true);
                    });
                }
                catch (InvalidOperationException) { done.TrySetResult(false); }
            });
            return done.Task;
        }

        string ModStatusCachePath() => Path.Combine(_buildDir, "work", "modstatus.json");

        // The full status check walks every mod, so it arrives late. Paint the saved
        // list (or mods.json) before the window is shown, then let status refresh it.
        void LoadInstantMods()
        {
            string cache = ModStatusCachePath();
            string index = Path.Combine(_modsDir, "mods.json");
            try
            {
                bool cacheFresh = File.Exists(cache) &&
                    (!File.Exists(index) || File.GetLastWriteTimeUtc(cache) >= File.GetLastWriteTimeUtc(index));
                if (cacheFresh)
                {
                    ApplyModStatus(0, File.ReadAllText(cache), persist: false);
                    return;
                }
            }
            catch { }
            SeedModsFromIndex();
        }

        void SeedModsFromIndex()
        {
            string index = Path.Combine(_modsDir, "mods.json");
            if (!File.Exists(index)) return;
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(index));
                var root = doc.RootElement;
                if (!root.TryGetProperty("mods", out var mods) || mods.ValueKind != JsonValueKind.Object) return;
                var order = new List<string>();
                if (root.TryGetProperty("order", out var ord) && ord.ValueKind == JsonValueKind.Array)
                {
                    foreach (var n in ord.EnumerateArray())
                        if (n.ValueKind == JsonValueKind.String) order.Add(n.GetString() ?? "");
                }
                foreach (var prop in mods.EnumerateObject())
                    if (!order.Contains(prop.Name)) order.Add(prop.Name);
                var list = new List<ModInfo>();
                foreach (string name in order)
                {
                    if (name.Length == 0 || !mods.TryGetProperty(name, out var m) || m.ValueKind != JsonValueKind.Object) continue;
                    string title = GetString(m, "title");
                    string gb = "";
                    if (m.TryGetProperty("gamebanana", out var g))
                        gb = g.ValueKind == JsonValueKind.Number ? g.GetInt64().ToString() : g.GetString() ?? "";
                    list.Add(new ModInfo
                    {
                        Name = name,
                        Title = title.Length > 0 ? title : name,
                        Enabled = !m.TryGetProperty("enabled", out var en) || en.ValueKind != JsonValueKind.False,
                        Author = GetString(m, "author"),
                        Category = GetString(m, "category"),
                        GamebananaId = gb,
                    });
                }
                _mods = list;
                BuildModLines();
                _modsChecked = true;
            }
            catch { }
        }

        // ---------------------------------------------------------------------------
        // Mods copied into mods\ by hand (Explorer, a ZIP, another PC).
        string KnownModsPath() => Path.Combine(_buildDir, "work", "known_mods.txt");

        void ScanForNewMods()
        {
            try
            {
                if (!Directory.Exists(_modsDir)) return;
                var present = Directory.GetDirectories(_modsDir)
                    .Select(Path.GetFileName)
                    .Where(n => !string.IsNullOrEmpty(n) && !n!.StartsWith("."))
                    .Select(n => n!)
                    .ToList();
                var known = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                string listPath = KnownModsPath();
                bool seeded = File.Exists(listPath);
                if (seeded)
                {
                    foreach (string line in File.ReadAllLines(listPath))
                    {
                        string n = line.Trim();
                        if (n.Length > 0 && present.Contains(n, StringComparer.OrdinalIgnoreCase)) known.Add(n);
                    }
                }
                else
                {
                    known.UnionWith(present);
                }
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(listPath)!);
                    File.WriteAllLines(listPath, known.OrderBy(n => n, StringComparer.OrdinalIgnoreCase));
                }
                catch { }
                if (!seeded) return;
                try
                {
                    string index = Path.Combine(_modsDir, "mods.json");
                    if (File.Exists(index))
                    {
                        using var doc = JsonDocument.Parse(File.ReadAllText(index));
                        if (doc.RootElement.TryGetProperty("mods", out var mods) && mods.ValueKind == JsonValueKind.Object)
                            foreach (var prop in mods.EnumerateObject()) known.Add(prop.Name);
                    }
                }
                catch { }
                var found = present.Where(n => !known.Contains(n)).ToList();
                if (found.Count == 0) return;
                _newMods.Clear();
                _newMods.AddRange(found.OrderBy(n => n, StringComparer.OrdinalIgnoreCase));
                _newModsPending = true;
                Invalidate();
            }
            catch { }
        }

        bool ModHasPlugin(string name)
        {
            try
            {
                string folder = Path.Combine(_modsDir, name);
                if (!Directory.Exists(folder)) return false;
                foreach (string file in Directory.EnumerateFiles(folder, "*.nro", SearchOption.AllDirectories))
                {
                    string rel = Path.GetRelativePath(folder, file).Replace('\\', '/');
                    if (!rel.StartsWith("prebuilt/", StringComparison.OrdinalIgnoreCase)) return true;
                }
            }
            catch { }
            return false;
        }

        void RememberNewMods(IEnumerable<string> names)
        {
            try
            {
                var known = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                string listPath = KnownModsPath();
                if (File.Exists(listPath))
                    foreach (string line in File.ReadAllLines(listPath))
                        if (line.Trim().Length > 0) known.Add(line.Trim());
                known.UnionWith(names);
                Directory.CreateDirectory(Path.GetDirectoryName(listPath)!);
                File.WriteAllLines(listPath, known.OrderBy(n => n, StringComparer.OrdinalIgnoreCase));
            }
            catch { }
        }

        async Task BuildNewModsAsync()
        {
            if (_newModsBusy) return;
            _newModsPrompt = false;
            _newModsPending = false;
            if (_newMods.Count == 0) return;
            string[] names = _newMods.ToArray();
            _newMods.Clear();
            _newModsBusy = true;
            _pluginBuildUi = true;
            _pluginFrac = -1;
            _pluginDetail = names.Length == 1 ? "Building the new mod's plugin" : "Building the new mods' plugins";
            _pluginEta = "";
            Invalidate();
            string why = "";
            try
            {
                var args = new List<string> { "enable" };
                args.AddRange(names);
                await Task.Run(() => OsMods(args.ToArray(), line =>
                {
                    if (!line.StartsWith("LOG ")) return;
                    string text = line.Substring(4).Trim();
                    BeginInvoke(() => NotePluginBuild(text));
                }));
                RememberNewMods(names);
            }
            catch (Exception ex) { why = ex.Message; }
            finally
            {
                _newModsBusy = false;
                _pluginBuildUi = false;
                UpdateMods();
                _ = PushInstalledState();
                Invalidate();
                PlayDone();
            }
            if (why.Length > 0) MessageBox.Show(why, "OpenSmash");
        }

        void MaybeShowBootPrompts()
        {
            if (_shownAt < 0 || Now - _shownAt < 0.8) return;
            if (_newModsPrompt || _updatePrompt || _shaderPrompt || _pluginPrompt || _rebuildPrompt || _sharePrompt || _checkModsPrompt || _showSetupPrompt) return;
            if (_newModsPending)
            {
                _newModsPending = false;
                _newModsPrompt = true;
                PlayPopup();
                Invalidate();
                return;
            }
            if (_updatePromptPending)
            {
                _updatePromptPending = false;
                _updatePrompt = true;
                _updateDontShow = false;
                PlayPopup();
                Invalidate();
            }
        }

        static string ShortCommit(string sha) => string.IsNullOrEmpty(sha) ? "?" : (sha.Length > 7 ? sha.Substring(0, 7) : sha);
        static string ShortError(string text)
        {
            text = (text ?? "").Replace("\r", " ").Replace("\n", " ").Trim();
            return text.Length <= 180 ? text : text.Substring(0, 177) + "\u2026";
        }

        // The tools' own Python (deps\venv): an install from before it has none, one whose venv's Python was
        // uninstalled has one that no longer runs, and one whose pip failed (offline) has no packages. The installer's
        // Python step makes it or adds them (tools\install_windowsmash.ps1 -PythonOnly); a PC with deps\python (it had
        // no Python) keeps that.
        async Task EnsureOwnPythonAsync()
        {
            // (an install on a Python of the PC's keeps it until deps\venv has its packages: .opensmash-ready)
            try
            {
                string ready = Path.Combine(_repo, "deps", "venv", ".opensmash-ready");
                string bundled = Path.Combine(_repo, "deps", "python", "python.exe");
                bool ok = await Task.Run(() => File.Exists(ready) || (File.Exists(bundled) && PythonHasPackages(bundled)));
                if (!ok) await RunPythonStepAsync("SETTING UP PYTHON");
            }
            catch { }
        }

        async Task RunPythonStepAsync(string label)
        {
            string script = Path.Combine(_repo, "tools", "install_windowsmash.ps1");
            if (!File.Exists(script)) return;
            SetLoad(label, -1);
            try
            {
                var r = await Task.Run(() => RunPowerShellUpdate(script, "-PythonOnly"));
                if (r.Code != 0) try { File.AppendAllText(Path.Combine(Path.GetTempPath(), "os_update.log"), r.Output + "\n---\n"); } catch { }
            }
            catch { }
            finally { ClearLoad(); }
            _pythonExe = ModManagerProcess.FindPython(_repo, _buildDir);
        }

        // (pycryptodomex alone: pip installs requirements.txt all at once or not at all)
        static bool PythonHasPackages(string exe)
        {
            try
            {
                var psi = new ProcessStartInfo { FileName = exe, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                psi.ArgumentList.Add("-c"); psi.ArgumentList.Add("import Cryptodome");
                using var p = Process.Start(psi);
                if (p == null) return false;
                _ = p.StandardOutput.ReadToEndAsync(); _ = p.StandardError.ReadToEndAsync();
                if (!p.WaitForExit(15000)) { try { p.Kill(); } catch { } return false; }
                return p.ExitCode == 0;
            }
            catch { return false; }
        }

        async Task CheckUpdatesAtBootAsync()
        {
            await Task.Delay(1600);
            if (!IsDisposed) await CheckForUpdatesAsync(true);
        }

        async Task CheckForUpdatesAsync(bool atBoot)
        {
            if (_updateChecking || _updateApplying) return;
            _updateChecking = true;
            _updateState = "checking";
            _updateMessage = "Checking GitHub for a newer version\u2026";
            _updateDetail = "";
            Invalidate();
            try
            {
                string local = await Task.Run(FindLocalCommit);
                _updateLocal = local;
                string headBody = await GitHubGetAsync($"{UpdateApi}/commits/main");
                using var headDoc = JsonDocument.Parse(headBody);
                var head = headDoc.RootElement;
                string sha = GetString(head, "sha").Trim();
                string title = "";
                if (head.TryGetProperty("commit", out var commit) && commit.TryGetProperty("message", out var message))
                    title = (message.GetString() ?? "").Split('\n')[0].Trim();
                _updateHead = sha;
                if (sha.Length == 0) throw new InvalidOperationException("GitHub did not say which version main is.");
                if (local.Length == 0)
                {
                    _updateState = "error";
                    _updateMessage = "This copy doesn't say which version it is, so an update can't be checked.";
                    _updateDetail = "tools\\source_version.txt names no commit and this is not a git checkout.";
                    return;
                }
                if (local.Equals(sha, StringComparison.OrdinalIgnoreCase))
                {
                    _updateState = "uptodate";
                    _updateBehind = 0;
                    _updateMessage = $"OpenSmash is up to date ({ShortCommit(local)}).";
                    _updateDetail = title;
                    return;
                }
                string cmpBody;
                try
                {
                    cmpBody = await GitHubGetAsync($"{UpdateApi}/compare/{local}...{sha}");
                }
                catch (Exception ex)
                {
                    _updateState = "error";
                    _updateMessage = "This copy's commit isn't on GitHub's main branch, so updates can't be checked. (Normal for a development checkout.)";
                    _updateDetail = ShortError(ex.Message);
                    return;
                }
                using var cmpDoc = JsonDocument.Parse(cmpBody);
                var cmp = cmpDoc.RootElement;
                string status = GetString(cmp, "status");
                int ahead = cmp.TryGetProperty("ahead_by", out var ab) && ab.TryGetInt32(out int n) ? n : 0;
                if (status == "ahead")
                {
                    _updateState = "available";
                    _updateBehind = ahead;
                    _updateMessage = ahead == 1 ? "A newer version is on GitHub: 1 new commit." : $"A newer version is on GitHub: {ahead} new commits.";
                    var titles = new List<string>();
                    if (cmp.TryGetProperty("commits", out var commits) && commits.ValueKind == JsonValueKind.Array)
                    {
                        foreach (var c in commits.EnumerateArray())
                        {
                            if (!c.TryGetProperty("commit", out var cm) || !cm.TryGetProperty("message", out var mm)) continue;
                            string line = (mm.GetString() ?? "").Split('\n')[0].Trim();
                            if (line.Length == 0 || line.StartsWith("Merge pull request", StringComparison.OrdinalIgnoreCase) || line.StartsWith("Merge branch", StringComparison.OrdinalIgnoreCase)) continue;
                            titles.Add(line);
                        }
                    }
                    _updateDetail = titles.Count > 0 ? string.Join("   \u00B7   ", titles.Take(3)) + (titles.Count > 3 ? "   \u00B7   \u2026" : "") : title;
                }
                else if (status == "identical") { _updateState = "uptodate"; _updateBehind = 0; _updateMessage = $"OpenSmash is up to date ({ShortCommit(local)})."; _updateDetail = title; }
                else if (status == "behind") { _updateState = "uptodate"; _updateBehind = 0; _updateMessage = $"This copy ({ShortCommit(local)}) is newer than GitHub's main branch."; _updateDetail = ""; }
                else if (status == "diverged") { _updateState = "error"; _updateMessage = "This copy comes from a different branch than GitHub's main, so it can't be updated automatically."; _updateDetail = ""; }
                else { _updateState = "error"; _updateMessage = "GitHub's answer was not understood."; _updateDetail = status; }
            }
            catch (Exception ex) { _updateState = "error"; _updateMessage = "Could not check for updates: " + ShortError(ex.Message); _updateDetail = ""; try { System.IO.File.AppendAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "os_update.log"), ex.ToString() + "\n---\n"); } catch { } }
            finally
            {
                _updateChecking = false;
                if (atBoot && _updateState == "available" && UpdatePromptEnabled()) _updatePromptPending = true;
                Invalidate();
            }
        }

        async Task<string> GitHubGetAsync(string url)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            request.Headers.Accept.ParseAdd("application/vnd.github+json");
            using var response = await _httpClient.SendAsync(request);
            string body = await response.Content.ReadAsStringAsync();
            if (!response.IsSuccessStatusCode) throw new InvalidOperationException($"GitHub answered {(int)response.StatusCode} ({response.ReasonPhrase}) for {url}.");
            return body;
        }

        string FindLocalCommit()
        {
            try
            {
                string dotGit = Path.Combine(_repo, ".git");
                if (Directory.Exists(dotGit) || File.Exists(dotGit))
                {
                    // A git checkout is the commit it is on, changed or not: ApplyUpdate's script refuses a changed
                    // checkout itself ("changes of its own") instead of laying a download over it.
                    bool dirty = false;
                    if (!dirty)
                    {
                        var psi = new ProcessStartInfo { FileName = "git", WorkingDirectory = _repo, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                        psi.ArgumentList.Add("rev-parse"); psi.ArgumentList.Add("HEAD");
                        using var p = Process.Start(psi);
                        if (p != null)
                        {
                            var output = p.StandardOutput.ReadToEndAsync();
                            _ = p.StandardError.ReadToEndAsync();
                            if (p.WaitForExit(5000) && p.ExitCode == 0)
                            {
                                var m = Regex.Match(output.GetAwaiter().GetResult().Trim(), @"^[0-9a-fA-F]{40}$");
                                if (m.Success) return m.Value.ToLowerInvariant();
                            }
                            else { try { if (!p.HasExited) p.Kill(); } catch { } }
                        }
                    }
                }
            }
            catch { }
            try
            {
                string source = Path.Combine(_repo, "tools", "source_version.txt");
                if (File.Exists(source))
                {
                    var m = Regex.Match(File.ReadAllText(source), @"(?<![0-9a-fA-F])([0-9a-fA-F]{40})(?![0-9a-fA-F])");
                    if (m.Success) return m.Groups[1].Value.ToLowerInvariant();
                }
            }
            catch { }
            try
            {
                string version = Path.Combine(_repo, "version.txt");
                if (File.Exists(version))
                {
                    var m = Regex.Match(File.ReadAllText(version), @"(?m)^main\s+([0-9a-fA-F]{40})\b");
                    if (m.Success) return m.Groups[1].Value.ToLowerInvariant();
                }
            }
            catch { }
            return "";
        }

        bool UpdatePromptEnabled()
        {
            try { string v = IniValue(WindowSmash.ConfigFile(_repo, "launcher.ini"), "opensmash", "update_prompt"); return !string.Equals(v, "off", StringComparison.OrdinalIgnoreCase); }
            catch { return true; }
        }

        void SaveUpdatePromptPreference(bool show)
        {
            try { SetIni(WindowSmash.ConfigFile(_repo, "launcher.ini"), "opensmash", "update_prompt", show ? "on" : "off"); }
            catch { }
        }

        async Task ApplyUpdateAsync()
        {
            if (_updateApplying || _updateChecking) return;
            bool fromPrompt = _updatePrompt;
            if (fromPrompt && _updateDontShow) SaveUpdatePromptPreference(false);
            _updatePrompt = false;
            _updateApplying = true;
            _updateState = "updating";
            _updateMessage = "Updating OpenSmash\u2026";
            _updateDetail = "";
            SetLoad("UPDATING OPENSMASH", -1);
            Invalidate();
            string why = "";
            string output = "";
            try
            {
                string script = Path.Combine(_repo, "tools", "install_windowsmash.ps1");
                if (!File.Exists(script)) throw new FileNotFoundException("tools\\install_windowsmash.ps1 is missing.");
                var result = await Task.Run(() => RunPowerShellUpdate(script));
                output = result.Output;
                if (result.Code != 0) why = "The updater stopped (exit " + result.Code + ").";
                else await RunPythonStepAsync("UPDATING PYTHON PACKAGES");   // (an update can need a new package)
            }
            catch (Exception ex) { why = ex.Message; }
            finally
            {
                _updateApplying = false;
                ClearLoad();
                Invalidate();
            }
            _updateState = "checking";
            _updateMessage = "Checking the update\u2026";
            Invalidate();
            await CheckForUpdatesAsync(false);
            if (why.Length == 0 && _updateState != "uptodate") why = "The update did not finish. " + Tail(output);
            if (why.Length > 0)
            {
                if (output.Contains("changes of its own")) why = "This copy has local changes, so it cannot be updated automatically. Update it with git, or use a clean download.";
                _updateState = "error";
                _updateMessage = "Update failed: " + ShortError(why);
                _updateDetail = "";
                MessageBox.Show("The update could not be installed.\n\n" + why, "OpenSmash");
            }
            else
            {
                _updateState = "uptodate";
                _updateMessage = "Update installed. Press Play to rebuild what changed.";
                _updateDetail = "";
                MessageBox.Show("OpenSmash was updated. Your game files, settings, save and builds were left as they were.\n\nPress Play to rebuild what changed.", "OpenSmash");
            }
            PlayDone();
            Invalidate();
        }

        static (int Code, string Output) RunPowerShellUpdate(string script, string only = "-UpdateOnly")
        {
            string root = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(script)!, ".."));
            var psi = new ProcessStartInfo
            {
                FileName = "powershell.exe",
                WorkingDirectory = root,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            psi.ArgumentList.Add("-NoProfile");
            psi.ArgumentList.Add("-ExecutionPolicy");
            psi.ArgumentList.Add("Bypass");
            psi.ArgumentList.Add("-File");
            psi.ArgumentList.Add(script);
            psi.ArgumentList.Add(only);
            using var p = Process.Start(psi) ?? throw new InvalidOperationException("Could not start PowerShell.");
            var stdout = p.StandardOutput.ReadToEndAsync();
            var stderr = p.StandardError.ReadToEndAsync();
            p.WaitForExit();
            string output = (stdout.GetAwaiter().GetResult() + "\n" + stderr.GetAwaiter().GetResult()).Trim();
            return (p.ExitCode, output);
        }

         void ApplyModStatus(int code, string output, bool persist = true)
        {
            var list = new List<ModInfo>();
            var keep = new HashSet<string>();
            var pluginsSeen = new List<(string name, string version, bool fetched, string status, string source)>();
            bool parsed = false;
            bool modded = _modsModded;
            string reason = _pluginBuildReason;
            string vanillaReason = _vanillaBuildReason;
            string refresh = _pluginRefreshReason;
            string json = "";
            try
            {
                json = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).LastOrDefault(l => l.TrimStart().StartsWith("{")) ?? "";
                if (code == 0 && json.Length > 0)
                {
                    using var doc = JsonDocument.Parse(json);
                    var root = doc.RootElement;
                    modded = root.TryGetProperty("modded", out var md) && md.ValueKind == JsonValueKind.True;
                    reason = root.TryGetProperty("needs_build", out var nb) ? nb.GetString() ?? "" : "";
                    vanillaReason = root.TryGetProperty("needs_build_vanilla", out var nv) ? nv.GetString() ?? "" : reason;   // (older tools: needs_build)
                    refresh = root.TryGetProperty("needs_refresh", out var nr) ? nr.GetString() ?? "" : "";
                    if (root.TryGetProperty("plugins", out var plugins) && plugins.ValueKind == JsonValueKind.Array)
                    {
                        foreach (var p in plugins.EnumerateArray())
                        {
                            pluginsSeen.Add((
                                GetString(p, "name"),
                                GetString(p, "version"),
                                p.TryGetProperty("fetched", out var f) && f.ValueKind == JsonValueKind.True,
                                GetString(p, "status"),
                                GetString(p, "source")));
                        }
                    }
                    if (root.TryGetProperty("mods", out var mods) && mods.ValueKind == JsonValueKind.Array)
                    {
                        parsed = true;
                        foreach (var m in mods.EnumerateArray())
                        {
                            string name = GetString(m, "name");
                            string gb = "";
                            if (m.TryGetProperty("gamebanana", out var g))
                                gb = g.ValueKind == JsonValueKind.Number ? g.GetInt64().ToString() : g.GetString() ?? "";
                            string group = GetString(m, "group");
                            string groupThumb = GetString(m, "group_thumb");
                            string thumb = GetString(m, "thumb");
                            int files = m.TryGetProperty("files", out var fc) && fc.TryGetInt32(out int nfiles) ? nfiles : 0;
                            bool plugin = m.TryGetProperty("plugins", out var pl) && pl.ValueKind == JsonValueKind.Array && pl.GetArrayLength() > 0;
                            list.Add(new ModInfo
                            {
                                Name = name,
                                Title = GetString(m, "title"),
                                Files = files,
                                Plugin = plugin,
                                Enabled = m.TryGetProperty("enabled", out var en) && en.ValueKind == JsonValueKind.True,
                                GamebananaId = gb,
                                Group = group,
                                Part = GetString(m, "part"),
                                GroupTitle = GetString(m, "group_title"),
                                Author = GetString(m, "author"),
                                Category = GetString(m, "category"),
                                About = GetString(m, "about"),
                                Online = GetString(m, "online"),
                                NewSlots = m.TryGetProperty("new_slots", out var ns) && ns.ValueKind == JsonValueKind.True,
                                Code = GetString(m, "code"),
                                Pick = m.TryGetProperty("pick", out var pk) && pk.ValueKind == JsonValueKind.True,
                                Variant = GetString(m, "variant"),
                                Added = JsonInt(m, "added"),
                                Merged = JsonInt(m, "merged"),
                                Skipped = JsonInt(m, "skipped"),
                                KeptShared = JsonInt(m, "kept_shared"),
                            });
                            void TakeThumb(string key, string path)
                            {
                                if (path.Length == 0 || !File.Exists(path)) return;
                                keep.Add(key);
                                if (_modThumbs.ContainsKey(key)) return;
                                try
                                {
                                    using var fs = File.OpenRead(path);
                                    using var img = Image.FromStream(fs);
                                    _modThumbs[key] = new Bitmap(img);
                                }
                                catch { }
                            }
                            TakeThumb(name, thumb);
                            if (group.Length > 0) TakeThumb("g:" + group, groupThumb);
                        }
                    }
                }
            }
            catch { parsed = false; }
            if (!parsed) return;
            _modsModded = modded;
            _pluginBuildReason = reason;
            _vanillaBuildReason = vanillaReason;
            _pluginRefreshReason = refresh;
            _stdPlugins = pluginsSeen;
            foreach (string old in _modThumbs.Keys.Where(k => !keep.Contains(k)).ToList())
            {
                _modThumbs[old].Dispose();
                _modThumbs.Remove(old);
            }
            _mods = list;
            BuildModLines();
            _modsChecked = true;
            if (persist && json.Length > 0)
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(ModStatusCachePath())!);
                    File.WriteAllText(ModStatusCachePath(), json);
                }
                catch { }
            }
            if (_sel > ModActionCount + _modLines.Count - 1) _sel = Math.Max(0, ModActionCount + _modLines.Count - 1);
            Invalidate();
        }

        void BuildModLines()
        {
            var lines = new List<ModLine>();
            var seen = new HashSet<string>();
            foreach (var m in _mods)
            {
                if (string.IsNullOrEmpty(m.Group))
                {
                    lines.Add(DetailLine(m, string.IsNullOrEmpty(m.Title) ? m.Name : m.Title, m.Name));
                    continue;
                }
                if (!seen.Add(m.Group)) continue;
                var parts = _mods.Where(x => x.Group == m.Group).ToArray();
                int on = parts.Count(p => p.Enabled);
                string worst = parts.Select(p => p.Online).FirstOrDefault(o => o == "unsafe")
                    ?? parts.Select(p => p.Online).FirstOrDefault(o => o == "caution")
                    ?? parts.Select(p => p.Online).FirstOrDefault(o => o == "same")
                    ?? parts.Select(p => p.Online).FirstOrDefault(o => o == "safe")
                    ?? "";
                // UI color packs: one row + dropdown to pick the active variant (only one on).
                if (parts.Any(p => p.Pick))
                {
                    int active = Array.FindIndex(parts, p => p.Enabled);
                    if (active < 0) active = 0;
                    var activePart = parts[active];
                    var meta = new List<string>();
                    if (!string.IsNullOrEmpty(activePart.Category)) meta.Add(activePart.Category.ToUpperInvariant());
                    if (!string.IsNullOrEmpty(activePart.Author)) meta.Add("BY " + activePart.Author.ToUpperInvariant());
                    lines.Add(new ModLine
                    {
                        Pick = true,
                        GroupKey = m.Group,
                        Title = string.IsNullOrEmpty(m.GroupTitle) ? m.Group : m.GroupTitle,
                        Sub = parts.Length + " COLORS",
                        Files = activePart.Files,
                        Enabled = on > 0,
                        Thumb = "g:" + m.Group,
                        Names = parts.Select(p => p.Name).ToArray(),
                        Variants = parts.Select(p => string.IsNullOrEmpty(p.Variant)
                            ? (string.IsNullOrEmpty(p.Part) ? p.Name : p.Part)
                            : p.Variant).ToArray(),
                        Active = active,
                        Meta = string.Join("   ·   ", meta),
                        About = activePart.About ?? "",
                        Online = worst,
                        NewSlots = parts.Any(p => p.NewSlots),
                        Code = parts.Any(p => p.Code == "waiting") ? "waiting" : parts.Any(p => p.Code == "built") ? "built" : "",
                        Added = parts.Sum(p => p.Added),
                        Merged = parts.Sum(p => p.Merged),
                        Skipped = parts.Sum(p => p.Skipped),
                        KeptShared = parts.Sum(p => p.KeptShared),
                    });
                    continue;
                }
                if (_knownGroups.Add(m.Group)) _openGroups.Add(m.Group);
                bool open = _openGroups.Contains(m.Group);
                lines.Add(new ModLine
                {
                    Header = true,
                    Open = open,
                    Title = string.IsNullOrEmpty(m.GroupTitle) ? m.Group : m.GroupTitle,
                    Sub = parts.Length + " PARTS" + (on == parts.Length ? "" : "  ·  " + on + " ON"),
                    Files = parts.Sum(p => p.Files),
                    Enabled = on == parts.Length,
                    Mixed = on > 0 && on < parts.Length,
                    Thumb = "g:" + m.Group,
                    Names = parts.Select(p => p.Name).ToArray(),
                    Online = worst,
                    NewSlots = parts.Any(p => p.NewSlots),
                    Code = parts.Any(p => p.Code == "waiting") ? "waiting" : parts.Any(p => p.Code == "built") ? "built" : "",
                });
                if (!open) continue;
                foreach (var p in parts)
                    lines.Add(DetailLine(p, string.IsNullOrEmpty(p.Part) ? p.Name : p.Part, null, child: true));
            }
            foreach (var line in lines) line.RowH = ModLineHeight(line);
            _modLines = lines;
            if (_openDropdownKey != null && _openDropdownKey.StartsWith("modpick:")
                && !_modLines.Any(l => l.Pick && "modpick:" + l.GroupKey == _openDropdownKey))
                _openDropdownKey = null;
        }

        static int JsonInt(JsonElement m, string key) =>
            m.TryGetProperty(key, out var v) && v.TryGetInt32(out int n) ? n : 0;

        ModLine DetailLine(ModInfo m, string title, string thumb, bool child = false)
        {
            var meta = new List<string>();
            if (!string.IsNullOrEmpty(m.Category)) meta.Add(m.Category.ToUpperInvariant());
            if (!string.IsNullOrEmpty(m.Author)) meta.Add("BY " + m.Author.ToUpperInvariant());
            return new ModLine
            {
                Child = child,
                Title = title,
                Files = m.Files,
                Enabled = m.Enabled,
                Thumb = thumb ?? "",
                Names = new[] { m.Name },
                Meta = string.Join("   ·   ", meta),
                About = m.About ?? "",
                Online = m.Online ?? "",
                NewSlots = m.NewSlots,
                Code = m.Code ?? "",
                Added = m.Added,
                Merged = m.Merged,
                Skipped = m.Skipped,
                KeptShared = m.KeptShared,
            };
        }

        static int ModLineHeight(ModLine m)
        {
            int h = 22 + 22;
            if (!string.IsNullOrEmpty(m.Sub)) h += 18;
            if (m.HasChips || !string.IsNullOrEmpty(m.Meta)) h += 26;
            if (!string.IsNullOrEmpty(m.About)) h += 44;
            return Math.Max(68, h + 16);
        }

        // ---- Storage: where dump\ and build\ live (tools/data_location.py), and moving them to another drive ----
        string _storageText = "";
        bool _storageBusy;
        async Task StorageStatusAsync()
        {
            if (_storageBusy) return;
            _storageBusy = true; _storageText = "Looking…"; Invalidate();
            string output = "";
            int code = await Task.Run(() => RunPython(new[] { Path.Combine(_repo, "tools", "data_location.py") }, out output));
            _storageText = code == 0 ? output.Trim() : "Could not read where the folders are:\n" + output.Trim();
            _storageBusy = false; Invalidate();
        }
        async Task StorageMoveAsync()
        {
            if (_storageBusy) return;
            string target;
            using (var dlg = new FolderBrowserDialog { Description = "The folder on another drive for the game's files (dump) and the compiled game (build): tens of GB", UseDescriptionForTitle = true })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK || string.IsNullOrEmpty(dlg.SelectedPath)) return;
                target = dlg.SelectedPath;
            }
            if (MessageBox.Show(this, $"Move dump\\ and build\\ into {target}?\n\nClose the game first. A link is left in their place, so everything finds them where they were. This can take a while.",
                                "OpenSmash", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            _storageBusy = true; _storageText = "Moving the folders to " + target + "… (don't start the game)"; Invalidate();
            string output = "";
            int code = await Task.Run(() => RunPython(new[] { Path.Combine(_repo, "tools", "data_location.py"), "--move", target }, out output));
            _storageBusy = false;
            _storageText = (code == 0 ? "Moved.\n" : "The move stopped (what was moved was put back):\n") + output.Trim();
            Invalidate();
        }

        int OsMods(string[] args, Action<string> onLine = null)
        {
            var all = new List<string> { Path.Combine(_repo, "tools", "os_mods.py") };
            all.AddRange(args);
            int code = RunPython(all.ToArray(), out string output, onLine);
            ModManagerProcess.ValidateResult(code, output);
            return code;
        }

        // ---------------------------------------------------------------------------
        // Modding stack. Skyline + ARCropolis are the only things every mod needs,
        // so "setup modding" exists only while one of them is missing.
        bool ModdingInstalled =>
            (File.Exists(Path.Combine(_skylineDir, "exefs", "subsdk9")) ||
             File.Exists(Path.Combine(_repo, "sd", "ultimate", "skyline", "exefs", "subsdk9"))) &&
            File.Exists(Path.Combine(_pluginsDir, "libarcropolis.nro"));

        string[] PluginFileNames()
        {
            try
            {
                if (!Directory.Exists(_pluginsDir)) return Array.Empty<string>();
                return Directory.GetFiles(_pluginsDir, "*.nro")
                    .Select(Path.GetFileNameWithoutExtension)
                    .Where(n => !string.IsNullOrEmpty(n))
                    .ToArray()!;
            }
            catch { return Array.Empty<string>(); }
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
            _iniValues.Clear();
            _changed.Clear();
            string display = WindowSmash.ConfigFile(_repo, "display.ini");
            string pads = WindowSmash.ConfigFile(_repo, "controllers.ini");
            string D(string key, string fallback) => IniValue(display, "", key) ?? fallback;
            _iniValues["window"] = D("window", "windowed");
            _iniValues["width"] = D("width", "1280");
            _iniValues["height"] = D("height", "720");
            _iniValues["window_size"] = _iniValues["width"] + "x" + _iniValues["height"];
            _iniValues["pacing"] = D("pacing", "clock");
            _iniValues["render_scale"] = D("render_scale", "1");
            _iniValues["gpu"] = D("gpu", "auto");
            // (runtime/renderer.h's names: d3d11, which older display.ini files carry, is Direct3D 11)
            _iniValues["renderer"] = D("renderer", "auto").Trim().ToLowerInvariant() switch { "vulkan" or "vk" => "vulkan", "direct3d11" or "d3d11" or "direct3d" or "dx11" => "direct3d11", _ => "auto" };
            _iniValues["hide_cursor"] = D("hide_cursor", "1");
            _iniValues["overlay"] = D("overlay", "1");
            _iniValues["volume"] = D("volume", "100");
            _iniValues["keyboard_player"] = IniValue(pads, "general", "keyboard_player") ?? "1";
            _iniValues["background"] = IniValue(pads, "general", "background") ?? "1";
            string server = FirstLine(WindowSmash.ConfigFile(_repo, "online_server.txt"));
            string[] words = server.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            string host = words.Length > 0 ? words[0] : "";
            _iniValues["code"] = words.Length > 1 ? words[1] : "";
            const string Public = "smash.recomps.wtf:39421";
            if (host == "" || host.Equals("off", StringComparison.OrdinalIgnoreCase)) _iniValues["lobby"] = "offline";
            else if (host.Equals("auto", StringComparison.OrdinalIgnoreCase) || host.Equals("na", StringComparison.OrdinalIgnoreCase) || host.Equals("eu", StringComparison.OrdinalIgnoreCase)) _iniValues["lobby"] = "public";   // (auto: the nearest public lobby, the default)
            else if (host.Equals(Public, StringComparison.OrdinalIgnoreCase)) _iniValues["lobby"] = _iniValues["code"] == "" ? "public" : "private";
            else { _iniValues["lobby"] = "custom"; _iniValues["server"] = host; }
            if (!_iniValues.ContainsKey("server")) _iniValues["server"] = "";
            string launcherIni = WindowSmash.ConfigFile(_repo, "launcher.ini");
            string savedLobby = IniValue(launcherIni, "opensmash", "lobby");
            string savedCode = IniValue(launcherIni, "opensmash", "code");
            string savedServer = IniValue(launcherIni, "opensmash", "server");
            bool preferSavedLobby = false;
            if (File.Exists(launcherIni))
            {
                DateTime savedAt = File.GetLastWriteTimeUtc(launcherIni);
                string serverFile = WindowSmash.ConfigFile(_repo, "online_server.txt");
                DateTime serverAt = File.Exists(serverFile) ? File.GetLastWriteTimeUtc(serverFile) : DateTime.MinValue;
                preferSavedLobby = savedAt >= serverAt;
            }
            if (preferSavedLobby && savedLobby is "public" or "private" or "custom" or "offline")
                _iniValues["lobby"] = savedLobby;
            if (string.IsNullOrEmpty(_iniValues["code"]) && savedCode != null)
                _iniValues["code"] = savedCode;
            if ((preferSavedLobby || string.IsNullOrEmpty(_iniValues["server"])) && savedServer != null)
                _iniValues["server"] = savedServer;
            string direct = FirstLine(WindowSmash.ConfigFile(_repo, "online_direct.txt"));
            _iniValues["direct"] = direct.Equals("off", StringComparison.OrdinalIgnoreCase) ? "off" : "on";
            _iniValues["name"] = FirstLine(WindowSmash.ConfigFile(_repo, "online_name.txt"));
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

        bool SettingOn(Setting s)
        {
            string lobby = _iniValues.GetValueOrDefault("lobby", "public");
            if (s.Key is "name" or "direct") return lobby != "offline";
            return true;
        }

        string ShownValue(Setting s)
        {
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

        void CommitEdit()
        {
            if (_editingKey == null || _settingEdit == null || !_settingEdit.Visible) return;
            string typed = _settingEdit.Text.Trim();
            if (_iniValues.GetValueOrDefault(_editingKey, "") != typed) _changed.Add(_editingKey);
            _iniValues[_editingKey] = typed;
            _settingEdit.Visible = false;
            _editingKey = null;
            Invalidate();
            PersistSettings();
        }

        void BeginEdit(Setting s)
        {
            if (!SettingOn(s) || !_editRects.TryGetValue(s.Key, out var r)) return;
            CommitEdit();
            _editingKey = s.Key;
            _settingEdit.Text = _iniValues.GetValueOrDefault(s.Key, "");
            _editPlaced = Rectangle.Empty;
            FitSettingEdit(r);
            _settingEdit.Visible = true;
            _settingEdit.BringToFront();
            _settingEdit.Focus();
            _settingEdit.SelectAll();
        }

        // A change is written as soon as it is made, and again when the window closes.
        void WriteSetting(string key, string value)
        {
            _iniValues[key] = value;
            _changed.Add(key);
            if (key == "window_size" && value.Contains('x'))
            {
                var parts = value.Split('x');
                _iniValues["width"] = parts[0];
                _iniValues["height"] = parts[1];
                _changed.Add("width");
                _changed.Add("height");
            }
            PersistSettings();
        }

        bool _persisting;

        void PersistSettings()
        {
            if (!_settingsLoaded || _persisting) return;
            _persisting = true;
            try { WriteSettingsFiles(); }
            catch { }
            finally { _persisting = false; }
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
            // Only what was changed here since the files were read: the game's own settings menu (Esc)
            // writes these files too, while this window waits hidden for the game to close - writing
            // every value back when it closed put the game's changes (the renderer, the window...) back
            // to what they were before the game started.
            string display = WindowSmash.ConfigFile(_repo, "display.ini"), pads = WindowSmash.ConfigFile(_repo, "controllers.ini");
            foreach (var (key, fallback) in new[] { ("window", "windowed"), ("width", "1280"), ("height", "720"), ("pacing", "clock"),
                                                    ("render_scale", "1"), ("gpu", "auto"), ("renderer", "auto"), ("hide_cursor", "1"),
                                                    ("overlay", "1"), ("volume", "100") })
                if (_changed.Contains(key)) SetIni(display, "", key, _iniValues.GetValueOrDefault(key, fallback));
            foreach (var (key, fallback) in new[] { ("keyboard_player", "1"), ("background", "1") })
                if (_changed.Contains(key)) SetIni(pads, "general", key, _iniValues.GetValueOrDefault(key, fallback));
            bool serverChanged = _changed.Contains("lobby") || _changed.Contains("code") || _changed.Contains("server");
            if (serverReady && serverChanged)
                SetFirstLine(WindowSmash.ConfigFile(_repo, "online_server.txt"), server);
            if (_changed.Contains("name"))
                SetFirstLine(WindowSmash.ConfigFile(_repo, "online_name.txt"), _iniValues.GetValueOrDefault("name", "").Trim());
            if (_changed.Contains("direct"))
                SetFirstLine(WindowSmash.ConfigFile(_repo, "online_direct.txt"), _iniValues.GetValueOrDefault("direct", "on"));
            _changed.Clear();   // (written: the game's later changes stay)
            if (serverChanged && !serverReady) { _changed.Add("lobby"); _changed.Add("code"); _changed.Add("server"); }
            string launcherIni = WindowSmash.ConfigFile(_repo, "launcher.ini");
            SetIni(launcherIni, "opensmash", "lobby", lobby);
            SetIni(launcherIni, "opensmash", "code", code);
            SetIni(launcherIni, "opensmash", "server", host);
        }

        bool SaveGameSettings()
        {
            if (!_settingsLoaded) ReadSettings();
            CommitEdit();
            string lobby = _iniValues.GetValueOrDefault("lobby", "public");
            string code = _iniValues.GetValueOrDefault("code", "").Replace(" ", "");
            string host = _iniValues.GetValueOrDefault("server", "").Trim();
            if (lobby == "private" && code == "")
            {
                MessageBox.Show("Type a lobby code: a word your friends type too.", "OpenSmash");
                return false;
            }
            if (lobby == "custom" && host != "" && !System.Text.RegularExpressions.Regex.IsMatch(host, @"^\S+:\d+$"))
            {
                MessageBox.Show("Your own server is host:port, for example 192.168.1.20:39420.", "OpenSmash");
                return false;
            }
            WriteSettingsFiles();
            return true;
        }

        // ---------------------------------------------------------------------------
        // View switching
        void GoView(string name, bool record = true)
        {
            if (record && name != _view)
            {
                if (name == "home") _navBack.Clear();
                else _navBack.Push(_view);
            }
            if (_view == "settings" && name != "settings") CommitEdit();
            _view = name;
            _sel = 0;
            _hover = "";
            // Hide the WebView when leaving the marketplace. Entering is handled by
            // PaintModsBrowser, which sizes it before making it visible (no flicker).
            if (_marketplaceWebView != null && name != "mods_browser")
            {
                _marketplaceWebView.Visible = false;
            }
            
            if (name == "mods" || name == "setup_modding") UpdateMods();
            if (name == "settings" && !_settingsLoaded) ReadSettings();

            // The marketplace caches what is installed (plugins, loaders, mods) so its
            // requirement chips stay accurate; refresh it whenever it is opened.
            if (name == "mods_browser") _ = PushInstalledState();

            Invalidate();
        }

        void GoBack()
        {
            string prev = _navBack.Count > 0 ? _navBack.Pop() : "home";
            GoView(prev, record: false);
        }

        // ---------------------------------------------------------------------------
        // Hit-testing
        void AddHit(string id, float x, float y, float w, float h)
        {
            _hits.Add(new Hit { Id = id, X = x, Y = y, W = w, H = h });
        }

        string HitAt(int px, int py)
        {
            for (int i = _hits.Count - 1; i >= 0; i--)
            {
                var h = _hits[i];
                if (px >= h.X && px <= h.X + h.W && py >= h.Y && py <= h.Y + h.H) return h.Id;
            }
            return "";
        }

        // ---------------------------------------------------------------------------
        // Drawing helpers
        Brush Brush(Color c) => new SolidBrush(c);

        void DrawText(Graphics g, string text, Font font, Color color, float x, float y, float w, float h, StringAlignment align = StringAlignment.Near, StringAlignment valign = StringAlignment.Center)
            => Theme.DrawText(g, text, font, color, x, y, w, h, align, valign);

        void DrawTextSpaced(Graphics g, string text, Font font, Color color, float x, float y, float extra)
            => Theme.DrawTextSpaced(g, text, font, color, x, y, extra);

        GraphicsPath RoundRect(float x, float y, float w, float h, float r) => Theme.RoundRect(x, y, w, h, r);

        void FillAccentBar(Graphics g, float x, float y, float w, float h, float r, int alpha)
            => Theme.FillAccentBar(g, x, y, w, h, r, alpha);

        // ---------------------------------------------------------------------------
        // Size formatting
        string FormatSize(long bytes)
        {
            if (bytes < 1024) return $"{bytes} B";
            if (bytes < 1048576) return $"{(bytes / 1024.0):N0} KB";
            if (bytes < 1073741824) return $"{(bytes / 1048576.0):N0} MB";
            return $"{(bytes / 1073741824.0):N2} GB";
        }

        // ---------------------------------------------------------------------------
        // Mouse events
        void OnMouseMove(object sender, MouseEventArgs e)
        {
            if (_settingsThumbDrag && _settingsMaxScroll > 0 && _settingsTrackH > _settingsThumbH)
            {
                float travel = _settingsTrackH - _settingsThumbH;
                float frac = (e.Y - _settingsThumbGrab - _settingsTrackTop) / travel;
                ScrollSettings(Math.Clamp(frac, 0f, 1f) * _settingsMaxScroll);
            }
            if (_modThumbDrag && _modMaxScroll > 0 && _modTrackH > _modThumbH)
            {
                float travel = _modTrackH - _modThumbH;
                float frac = (e.Y - _modThumbGrab - _modTrackTop) / travel;
                _modScroll = (int)Math.Round(Math.Clamp(frac, 0f, 1f) * _modMaxScroll);
                Invalidate();
            }
            if (_volDragging && _volTrackValid && _volTrackW > 0)
            {
                float frac = (e.X - _volTrackX) / _volTrackW;
                frac = Math.Clamp(frac, 0f, 1f);
                int val = (int)Math.Round(frac * 100f);
                val = (int)(Math.Round(val / 5.0) * 5);
                _iniValues["volume"] = val.ToString();
                _changed.Add("volume");
                Invalidate();
            }
            string id = HitAt(e.X, e.Y);
            if (id != _hover)
            {
                _hover = id;
                _keys = false;
                bool beam = id != null && id.StartsWith("set:") && id.EndsWith(":dd")
                    && int.TryParse(id.Substring(4, id.Length - 7), out int beamIdx)
                    && beamIdx >= 0 && beamIdx < _settings.Length && _settings[beamIdx].Text;
                Cursor = string.IsNullOrEmpty(id) ? Cursors.Default : beam ? Cursors.IBeam : Cursors.Hand;

                // Track dropdown option hover
                if (id != null && id.StartsWith("set:") && id.Contains(":opt:"))
                {
                    var parts = id.Split(':');
                    _dropdownHoverIndex = int.Parse(parts[3]);
                }
                else if (id != null && id.StartsWith("modpick:opt:"))
                    _dropdownHoverIndex = int.Parse(id.Substring("modpick:opt:".Length));
                else if (id == null || (!id.StartsWith("modpick:") && !(id.StartsWith("set:") && id.Contains(":opt:"))))
                    _dropdownHoverIndex = -1;
                Invalidate();
            }
            else if (!string.IsNullOrEmpty(id))
                _keys = false;
        }

        void OnMouseLeave(object sender, EventArgs e)
        {
            if (!string.IsNullOrEmpty(_hover) || _pressed != "")
            {
                _hover = "";
                _pressed = "";
                _keys = false;
                Cursor = Cursors.Default;
                Invalidate();
            }
        }

        void OnMouseDown(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                string id = HitAt(e.X, e.Y);
                if (!string.IsNullOrEmpty(id) && id != "setscroll" && id != "modscroll" && !id.StartsWith("vol:"))
                {
                    if (_pressed != id) { _pressed = id; Invalidate(); }
                }
                if (id == "setscroll")
                {
                    Tag = id;
                    _settingsThumbDrag = true;
                    float thumbY = _settingsTrackTop;
                    if (_settingsMaxScroll > 0 && _settingsTrackH > _settingsThumbH)
                        thumbY += (_settingsTrackH - _settingsThumbH) * (_settingsScroll / _settingsMaxScroll);
                    if (e.Y < thumbY || e.Y > thumbY + _settingsThumbH)
                    {
                        float travel = Math.Max(1, _settingsTrackH - _settingsThumbH);
                        float frac = (e.Y - _settingsThumbH / 2f - _settingsTrackTop) / travel;
                        ScrollSettings(Math.Clamp(frac, 0f, 1f) * _settingsMaxScroll);
                        _settingsThumbGrab = _settingsThumbH / 2f;
                    }
                    else
                        _settingsThumbGrab = e.Y - thumbY;
                }
                else if (id == "modscroll")
                {
                    Tag = id;
                    _modThumbDrag = true;
                    float thumbY = _modTrackTop;
                    if (_modMaxScroll > 0 && _modTrackH > _modThumbH)
                        thumbY += (_modTrackH - _modThumbH) * _modScroll / _modMaxScroll;
                    if (e.Y < thumbY || e.Y > thumbY + _modThumbH)
                    {
                        float travel = Math.Max(1, _modTrackH - _modThumbH);
                        float frac = (e.Y - _modThumbH / 2f - _modTrackTop) / travel;
                        _modScroll = (int)Math.Round(Math.Clamp(frac, 0f, 1f) * _modMaxScroll);
                        _modThumbGrab = _modThumbH / 2f;
                        Invalidate();
                    }
                    else
                        _modThumbGrab = e.Y - thumbY;
                }
                else if (!string.IsNullOrEmpty(id))
                {
                    Tag = id;
                    if (id.StartsWith("vol:")) _volDragging = true;
                }
                else
                {
                    // Drag window
                    ReleaseCapture();
                    SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);
                }
            }
            else if (e.Button == MouseButtons.Right && _view == "settings")
            {
                string id = HitAt(e.X, e.Y);
                if (id.StartsWith("set:"))
                {
                    int idx = int.Parse(id.Substring(4));
                    CycleSetting(idx, -1);
                    Invalidate();
                }
            }
        }

        void OnMouseUp(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && Tag is string id && HitAt(e.X, e.Y) == id)
            {
                _lastClickX = e.X;
                ActivateHit(id);
            }
            if (_volDragging)
                WriteSetting("volume", _iniValues.GetValueOrDefault("volume", "100"));
            _volDragging = false;
            _settingsThumbDrag = false;
            _modThumbDrag = false;
            Tag = null;
            if (_pressed != "") { _pressed = ""; Invalidate(); }
        }

        void OnMouseWheel(object sender, MouseEventArgs e)
        {
            if (_view == "mods")
            {
                if (_openDropdownKey != null && _openDropdownKey.StartsWith("modpick:"))
                {
                    _ddFirst -= Math.Sign(e.Delta);
                    Invalidate();
                    return;
                }
                float step = _modLines.Count > 0 ? _modLines[0].RowH : 68f;
                _modScroll = (int)Math.Clamp(_modScroll - Math.Sign(e.Delta) * step, 0, Math.Max(0, _modMaxScroll));
                Invalidate();
            }
        }

        // Borderless + ShowInTaskbar still needs APPWINDOW or the button can stay missing until click.
        protected override CreateParams CreateParams
        {
            get
            {
                const int WsExAppWindow = 0x00040000;
                var cp = base.CreateParams;
                cp.ExStyle |= WsExAppWindow;
                return cp;
            }
        }

        float Press(string id) => _pressed == id ? 1f : 0f;

        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
        static extern IntPtr SendMessageStr(IntPtr hWnd, int msg, IntPtr wParam, string lParam);
        [DllImport("user32.dll", EntryPoint = "SendMessageW")]
        static extern IntPtr SendMessageRect(IntPtr hWnd, int msg, IntPtr wParam, ref RECT lParam);
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }

        Rectangle _editPlaced;
        float _editPlacedS;

        // The field is the same slanted box as the other settings. The text sits inside it.
        void FitSettingEdit(Rectangle r)
        {
            if (_settingEdit == null || r.Width < 4 || r.Height < 4) return;
            var font = FM(13 * _s);
            if (_settingEdit.Font != font) _settingEdit.Font = font;
            if (_settingEdit.Bounds != r) _settingEdit.Bounds = r;
            if (_editPlaced == r && _editPlacedS == _s && _settingEdit.Region != null) return;
            _editPlaced = r;
            _editPlacedS = _s;
            float cut = 8 * _s;
            float inset = Math.Max(1.5f, 1.5f * _s);
            _settingEdit.Region?.Dispose();
            _settingEdit.Region = new Region(Theme.Para(inset, inset, r.Width - inset * 2, r.Height - inset * 2, Math.Max(1, cut - inset)));
            int pad = (int)Math.Ceiling(cut + 8 * _s);
            int textH = TextRenderer.MeasureText("Ag", font, Size.Empty, TextFormatFlags.NoPadding).Height;
            int top = Math.Max(2, (r.Height - textH) / 2);
            var rc = new RECT { Left = pad, Top = top, Right = r.Width - pad, Bottom = top + textH + 2 };
            SendMessageRect(_settingEdit.Handle, 0xB4, IntPtr.Zero, ref rc);
        }

        // ---------------------------------------------------------------------------
        // Keyboard
        void OnKeyDown(object sender, KeyEventArgs e)
        {
            var nav = new[] { Keys.Up, Keys.Down, Keys.Left, Keys.Right };
            if (nav.Contains(e.KeyCode)) { _keys = true; _hover = ""; }

            if (_newModsPrompt)
            {
                if (e.KeyCode == Keys.Enter) _ = BuildNewModsAsync();
                return;
            }

            if (_updatePrompt)
            {
                if (e.KeyCode == Keys.Escape)
                {
                    if (_updateDontShow) SaveUpdatePromptPreference(false);
                    _updatePrompt = false;
                    _updatePromptPending = false;
                    Invalidate();
                }
                if (e.KeyCode == Keys.Enter && !_updateApplying) _ = ApplyUpdateAsync();
                return;
            }

            if (_rebuildPrompt)
            {
                if (e.KeyCode == Keys.Escape) { _rebuildPrompt = false; Invalidate(); }
                if (e.KeyCode == Keys.Enter) { _rebuildPrompt = false; Invalidate(); _ = RefreshPlugins(); }
                return;
            }

            if (_sharePrompt)
            {
                if (e.KeyCode == Keys.Escape) { _sharePrompt = false; Invalidate(); }
                if (e.KeyCode == Keys.Enter) { _sharePrompt = false; Invalidate(); ShareModList(true); }
                return;
            }

            if (_checkModsPrompt)
            {
                if (e.KeyCode == Keys.Escape) { _checkModsPrompt = false; Invalidate(); }
                if (e.KeyCode == Keys.Enter)
                {
                    if (_checkModsUpdates.Count > 0) { _checkModsPrompt = false; Invalidate(); _ = InstallModUpdates(); }
                    else { _checkModsPrompt = false; Invalidate(); }
                }
                return;
            }

            if (_shaderPrompt)
            {
                if (e.KeyCode == Keys.Escape) { _shaderPrompt = false; Invalidate(); }
                return;
            }

            if (_pluginPrompt)
            {
                if (e.KeyCode == Keys.Escape) _pluginPromptWait?.TrySetResult("cancel");
                if (e.KeyCode == Keys.Enter) _pluginPromptWait?.TrySetResult(_pluginPromptKind == "refresh" ? "refresh" : "build");
                return;
            }

            if (_view == "home")
            {
                if (e.KeyCode == Keys.Up) { _sel = (_sel + _menuItems.Length - 1) % _menuItems.Length; Invalidate(); }
                if (e.KeyCode == Keys.Down) { _sel = (_sel + 1) % _menuItems.Length; Invalidate(); }
                if (e.KeyCode == Keys.Enter) ActivateHit($"menu:{_sel}");
                if (e.KeyCode == Keys.Escape) Close();
            }
            else if (_view == "mods")
            {
                if (_showSetupPrompt)
                {
                    if (e.KeyCode == Keys.Escape)
                    {
                        _showSetupPrompt = false;
                        GoBack();
                        return;
                    }
                }
                else if (_openDropdownKey != null && _openDropdownKey.StartsWith("modpick:"))
                {
                    var line = _modLines.FirstOrDefault(l => l.Pick && "modpick:" + l.GroupKey == _openDropdownKey);
                    int n = line?.Variants?.Length ?? 0;
                    if (e.KeyCode == Keys.Up) { _dropdownHoverIndex = Math.Max(0, _dropdownHoverIndex - 1); Invalidate(); }
                    if (e.KeyCode == Keys.Down) { _dropdownHoverIndex = Math.Min(n - 1, Math.Max(0, _dropdownHoverIndex) + 1); Invalidate(); }
                    if (e.KeyCode == Keys.Enter && line != null && _dropdownHoverIndex >= 0)
                        SelectPickVariant(line, _dropdownHoverIndex);
                    if (e.KeyCode == Keys.Escape) { _openDropdownKey = null; Invalidate(); }
                }
                else
                {
                    int rows = ModActionCount + _modLines.Count;
                    if (e.KeyCode == Keys.Up && _sel > 0) { _sel--; EnsureModVisible(); Invalidate(); }
                    if (e.KeyCode == Keys.Down && _sel < rows - 1) { _sel++; EnsureModVisible(); Invalidate(); }
                    if (e.KeyCode == Keys.Delete && _sel >= ModActionCount) RemoveMod();
                    if (e.KeyCode == Keys.Enter && _sel < ModActionCount) ActivateHit(ModActionId(_sel));
                    if (e.KeyCode == Keys.Escape) GoBack();
                }
            }
            else if (_view == "settings")
            {
                if (e.KeyCode == Keys.Up) 
                { 
                    if (!string.IsNullOrEmpty(_openDropdownKey))
                    {
                        _dropdownHoverIndex = Math.Max(0, _dropdownHoverIndex - 1);
                        Invalidate();
                    }
                    else if (_sel > SectionStart(_settingsTab)) { _sel--; EnsureSettingsVisible(); Invalidate(); }
                }
                if (e.KeyCode == Keys.Down)
                {
                    if (!string.IsNullOrEmpty(_openDropdownKey))
                    {
                        var setting = GetVisibleSetting(_sel);
                        if (setting != null)
                            _dropdownHoverIndex = Math.Min(setting.Values.Length - 1, _dropdownHoverIndex + 1);
                        Invalidate();
                    }
                    else if (_sel < SectionStart(_settingsTab) + (_settingsTab < _settingsTabs.Length ? _settingsTabs[_settingsTab].SettingKeys.Length : 0) - 1)
                    { _sel++; EnsureSettingsVisible(); Invalidate(); }
                }
                if (e.KeyCode == Keys.Enter)
                {
                    if (!string.IsNullOrEmpty(_openDropdownKey) && _dropdownHoverIndex >= 0)
                    {
                        ApplyDropdownSelection();
                    }
                    else if (_sel < GetVisibleSettingsCount())
                    {
                        var set = GetVisibleSetting(_sel);
                        if (set != null && set.Text) BeginEdit(set);
                        else if (set == null || !set.Slider) ToggleDropdown(_sel);
                    }
                    else if (_settingsTab < _settingsTabs.Length
                        && string.Equals(_settingsTabs[_settingsTab].Name, "Update", StringComparison.OrdinalIgnoreCase)
                        && !_updateChecking && !_updateApplying)
                    {
                        if (_updateState == "available") _ = ApplyUpdateAsync();
                        else _ = CheckForUpdatesAsync(false);
                    }
                }
                if (e.KeyCode == Keys.Escape)
                {
                    if (!string.IsNullOrEmpty(_openDropdownKey))
                        CloseDropdown();
                    else
                        GoBack();
                }
                bool ctrl = (Control.ModifierKeys & Keys.Control) == Keys.Control;
                if (e.KeyCode == Keys.Left)
                {
                    if (ctrl && _sel < GetVisibleSettingsCount()) { CycleSetting(GetVisibleSettingIndex(_sel), -1); Invalidate(); }
                    else ShowSettingsSection(_settingsTab - 1);
                }
                if (e.KeyCode == Keys.Right)
                {
                    if (ctrl && _sel < GetVisibleSettingsCount()) { CycleSetting(GetVisibleSettingIndex(_sel), 1); Invalidate(); }
                    else ShowSettingsSection(_settingsTab + 1);
                }
            }
            else if (e.KeyCode == Keys.Escape) GoBack();
        }

        // ---------------------------------------------------------------------------
        // Actions
        void ActivateHit(string id)
        {
            if (_openDropdownKey != null && _openDropdownKey.StartsWith("modpick:") && !id.StartsWith("modpick:"))
                _openDropdownKey = null;
            if (ActivateNavHit(id)) return;
            if (id.StartsWith("menu:"))
            {
                int i = int.Parse(id.Substring(5));
                if (i == 0) StartPlay();
                else if (i == 1) GoView("mods");
                else if (i == 2) GoView("settings");
                else Close();
            }
            else if (id == "back") GoBack();
            else if (id == "close") Close();
            else if (id == "act:browse") GoView("mods_browser");
            else if (id == "act:setup") GoView("setup_modding");
            else if (id == "act:check") _ = CheckMods();
            else if (id == "act:share") { _sharePrompt = true; PlayPopup(); Invalidate(); }
            else if (id == "act:build") _ = BuildPlugins();
            else if (id == "act:open" && Directory.Exists(_modsDir))
                Process.Start("explorer.exe", $"\"{_modsDir}\"");
            else if (id == "act:add") AddMod();
            else if (id == "act:remove") RemoveMod();
            else if (id == "act:refresh") { UpdateMods(); _ = PushInstalledState(); Invalidate(); }
            else if (id == "act:rebuild") { _rebuildPrompt = true; PlayPopup(); Invalidate(); }
            else if (id == "setup_mode:scratch")
            {
                _setupMode = "scratch";
                _ = RunSetupModdingAsync();
            }
            else if (id == "setup_mode:import")
            {
                _setupMode = "import";
                _foundEmulators = FindEmulators();
                _importSel = 0;
                Invalidate();
            }
            else if (id.StartsWith("emu:"))
            {
                int i = int.Parse(id.Substring(4));
                if (i >= 0 && i < _foundEmulators.Count)
                    _importSel = i;
                Invalidate();
            }
            else if (id == "setup_start_import")
            {
                if (_importSel >= 0 && _importSel < _foundEmulators.Count)
                    _ = ImportFromEmulator(_foundEmulators[_importSel]);
            }
            else if (id == "setup_start") _ = FetchPlugins(false);
            else if (id == "setup_again") _ = FetchPlugins(true);
            else if (id == "setup_add")
            {
                using var d = new OpenFileDialog { Filter = "Plugin (*.nro)|*.nro|All files|*.*", Multiselect = true };
                if (d.ShowDialog(this) != DialogResult.OK) return;
                string folder = Path.Combine(_repo, "skyline", "plugins");
                Directory.CreateDirectory(folder);
                foreach (var src in d.FileNames)
                {
                    try { File.Copy(src, Path.Combine(folder, Path.GetFileName(src)), true); }
                    catch (Exception ex) { MessageBox.Show($"Could not copy {Path.GetFileName(src)}: {ex.Message}", "OpenSmash"); }
                }
                UpdateMods();
                _ = PushInstalledState();
                Invalidate();
            }
            else if (id.StartsWith("setup_import:"))
            {
                int ei = int.Parse(id.Substring("setup_import:".Length));
                var emus = FindEmulators();
                if (ei >= 0 && ei < emus.Count)
                {
                    _foundEmulators = emus;
                    _importSel = ei;
                    _ = ImportFromEmulator(emus[ei]);
                }
            }
            else if (id == "plugin_prompt:build") _pluginPromptWait?.TrySetResult(_pluginPromptKind == "refresh" ? "refresh" : "build");
            else if (id == "plugin_prompt:play") _pluginPromptWait?.TrySetResult("play");
            else if (id == "plugin_prompt:cancel") _pluginPromptWait?.TrySetResult("cancel");
            else if (id == "plugin_prompt:block") { }
            else if (id == "newmods:build") _ = BuildNewModsAsync();
            else if (id == "newmods:block") { }
            else if (id == "update_prompt:dont")
            {
                _updateDontShow = !_updateDontShow;
                Invalidate();
            }
            else if (id == "update_prompt:update") _ = ApplyUpdateAsync();
            else if (id == "update_prompt:later")
            {
                if (_updateDontShow) SaveUpdatePromptPreference(false);
                _updatePrompt = false;
                _updatePromptPending = false;
                Invalidate();
            }
            else if (id == "update_prompt:block") { }
            else if (id == "update:check") _ = CheckForUpdatesAsync(false);
            else if (id == "storage:refresh") _ = StorageStatusAsync();
            else if (id == "storage:move") _ = StorageMoveAsync();
            else if (id == "update:apply") _ = ApplyUpdateAsync();
            else if (id == "rebuild_prompt:cancel") { _rebuildPrompt = false; Invalidate(); }
            else if (id == "rebuild_prompt:refresh") { _rebuildPrompt = false; Invalidate(); _ = RefreshPlugins(); }
            else if (id == "rebuild_prompt:update") { _rebuildPrompt = false; Invalidate(); _ = BuildPlugins(); }
            else if (id == "rebuild_prompt:block") { }
            else if (id == "share_prompt:cancel") { _sharePrompt = false; Invalidate(); }
            else if (id == "share_prompt:share") { _sharePrompt = false; Invalidate(); ShareModList(true); }
            else if (id == "share_prompt:install") { _sharePrompt = false; Invalidate(); ShareModList(false); }
            else if (id == "share_prompt:block") { }
            else if (id == "check_prompt:close") { _checkModsPrompt = false; Invalidate(); }
            else if (id == "check_prompt:install") { _checkModsPrompt = false; Invalidate(); _ = InstallModUpdates(); }
            else if (id == "check_prompt:block") { }
            else if (id == "shader_prompt:low") StartShaders("low");
            else if (id == "shader_prompt:medium") StartShaders("medium");
            else if (id == "shader_prompt:high") StartShaders("high");
            else if (id == "shader_prompt:later" || id == "shader_prompt:block") _shaderPrompt = false;
            else if (id == "setup_prompt:setup")
            {
                _showSetupPrompt = false;
                GoView("setup_modding");
            }
            else if (id == "setup_prompt:cancel")
            {
                _showSetupPrompt = false;
                GoView("home");
            }
            else if (id.StartsWith("modopen:"))
            {
                int index = int.Parse(id.Substring("modopen:".Length));
                if (index >= 0 && index < _modLines.Count && _modLines[index].Header)
                {
                    string key = _modLines[index].Thumb.StartsWith("g:") ? _modLines[index].Thumb.Substring(2) : "";
                    if (_openGroups.Contains(key)) _openGroups.Remove(key);
                    else _openGroups.Add(key);
                    BuildModLines();
                    Invalidate();
                }
            }
            else if (id.StartsWith("modpick:opt:"))
            {
                int vi = int.Parse(id.Substring("modpick:opt:".Length));
                var line = _modLines.FirstOrDefault(l => l.Pick && "modpick:" + l.GroupKey == _openDropdownKey);
                if (line != null) SelectPickVariant(line, vi);
            }
            else if (id.StartsWith("modpick:dd:"))
            {
                int index = int.Parse(id.Substring("modpick:dd:".Length));
                if (index >= 0 && index < _modLines.Count && _modLines[index].Pick)
                {
                    string key = "modpick:" + _modLines[index].GroupKey;
                    if (_openDropdownKey == key) { _openDropdownKey = null; }
                    else
                    {
                        _openDropdownKey = key;
                        _dropdownHoverIndex = _modLines[index].Active;
                        _ddFirst = -1;
                    }
                    Invalidate();
                }
            }
            else if (id.StartsWith("modtoggle:"))
            {
                int index = int.Parse(id.Substring("modtoggle:".Length));
                if (index >= 0 && index < _modLines.Count)
                {
                    var line = _modLines[index];
                    if (line.Pick) TogglePickMod(line);
                    else ToggleMod(line.Names);
                }
            }
            else if (id.StartsWith("modrow:"))
            {
                int row = int.Parse(id.Substring(7));
                int index = row - ModActionCount;
                if (row == _sel && index >= 0 && index < _modLines.Count)
                {
                    var line = _modLines[index];
                    if (line.Header)
                    {
                        string key = line.Thumb.StartsWith("g:") ? line.Thumb.Substring(2) : "";
                        if (_openGroups.Contains(key)) _openGroups.Remove(key);
                        else _openGroups.Add(key);
                        BuildModLines();
                    }
                    else if (line.Pick)
                    {
                        string key = "modpick:" + line.GroupKey;
                        if (_openDropdownKey == key) _openDropdownKey = null;
                        else { _openDropdownKey = key; _dropdownHoverIndex = line.Active; _ddFirst = -1; }
                    }
                    else if (line.Names.Length > 0)
                        Process.Start("explorer.exe", $"\"{Path.Combine(_modsDir, line.Names[0])}\"");
                }
                _sel = row;
                Invalidate();
            }
            else if (id.StartsWith("set:"))
            {
                // set:{idx}           -> toggle dropdown
                // set:{idx}:dd        -> open dropdown
                // set:{idx}:opt:{vi}  -> select option
                string rest = id.Substring(4);
                if (rest.Contains(":opt:"))
                {
                    var parts = rest.Split(':');
                    int idx = int.Parse(parts[0]);
                    int vi = int.Parse(parts[2]);
                    var setting = _settings[idx];
                    _iniValues[setting.Key] = setting.Values[vi];
                    WriteSetting(setting.Key, setting.Values[vi]);
                    CloseDropdown();
                    Invalidate();
                }
                else if (rest.EndsWith(":dd"))
                {
                    int idx = int.Parse(rest.Substring(0, rest.Length - 3));
                    var set = _settings[idx];
                    if (set.Text) { BeginEdit(set); return; }
                    if (set.Slider || set.Key == "pads" || !SettingOn(set)) return;
                    var visibleIdx = GetVisibleSettings().ToList().FindIndex(s => Array.IndexOf(_settings, s) == idx);
                    ToggleDropdown(visibleIdx);
                }
                else
                {
                    int idx = int.Parse(rest);
                    var set = _settings[idx];
                    if (!SettingOn(set)) return;
                    if (set.Key == "pads")
                    {
                        string pads = WindowSmash.ConfigFile(_repo, "controllers.ini");
                        if (File.Exists(pads)) Process.Start(new ProcessStartInfo("notepad.exe", "\"" + pads + "\"") { UseShellExecute = true });
                        else MessageBox.Show("controllers.ini is written the first time the game starts.", "OpenSmash");
                        return;
                    }
                    if (set.Slider) return;
                    if (set.Text) { BeginEdit(set); return; }
                    var visibleIdx = GetVisibleSettings().ToList().FindIndex(s => Array.IndexOf(_settings, s) == idx);
                    ToggleDropdown(visibleIdx);
                }
            }
            else if (id.StartsWith("vol:"))
            {
                if (_volTrackValid && _volTrackW > 0)
                {
                    float frac = (_lastClickX - _volTrackX) / _volTrackW;
                    frac = Math.Clamp(frac, 0f, 1f);
                    int val = (int)Math.Round(frac * 100f);
                    // snap to 5
                    val = (int)(Math.Round(val / 5.0) * 5);
                    _iniValues["volume"] = val.ToString();
                    WriteSetting("volume", val.ToString());
                    Invalidate();
                }
            }
            else if (id.StartsWith("tab:"))
            {
                ShowSettingsSection(int.Parse(id.Substring(4)));
            }
            // Setup Modding hits
            else if (id == "setup_start") _ = FetchPlugins(false);
            else if (id == "setup_again") _ = FetchPlugins(true);
        }

        int GetVisibleSettingsCount() => GetVisibleSettings().Length;

        int GetVisibleSettingIndex(int visibleIdx)
        {
            var visibleSettings = GetVisibleSettings();
            if (visibleIdx >= 0 && visibleIdx < visibleSettings.Length)
                return Array.IndexOf(_settings, visibleSettings[visibleIdx]);
            return -1;
        }

        int SectionStart(int tab)
        {
            int n = 0;
            for (int i = 0; i < tab && i < _settingsTabs.Length; i++)
                n += _settingsTabs[i].SettingKeys.Length;
            return n;
        }

        void ShowSettingsSection(int tab)
        {
            if (_settingsTabs.Length == 0) return;
            tab = (tab + _settingsTabs.Length) % _settingsTabs.Length;
            if (tab != _settingsTab) CommitEdit();
            _settingsTab = tab;
            _sel = SectionStart(tab);
            _openDropdownKey = null;
            _settingsScroll = 0;
            Invalidate();
        }

        void CycleSetting(int i, int dir)
        {
            var s = _settings[i];
            if (s.Text || s.Values.Length == 0 || !SettingOn(s)) return;
            var vals = s.Values;
            string cur = _iniValues.GetValueOrDefault(s.Key, vals[0]);
            int idx = Array.IndexOf(vals, cur);
            if (idx < 0) idx = 0;
            idx = (idx + dir + vals.Length) % vals.Length;
            _iniValues[s.Key] = vals[idx];
            WriteSetting(s.Key, vals[idx]);
        }

        Setting GetVisibleSetting(int visibleIdx)
        {
            var visibleSettings = GetVisibleSettings();
            if (visibleIdx >= 0 && visibleIdx < visibleSettings.Length)
                return visibleSettings[visibleIdx];
            return null;
        }

        Setting[] GetVisibleSettings()
        {
            var list = new List<Setting>();
            foreach (var tab in _settingsTabs)
                foreach (var key in tab.SettingKeys)
                {
                    var set = _settings.FirstOrDefault(s => s.Key == key);
                    if (set != null) list.Add(set);
                }
            return list.ToArray();
        }

        void ToggleDropdown(int visibleIdx)
        {
            var setting = GetVisibleSetting(visibleIdx);
            if (setting == null || setting.Text || setting.Slider || setting.Values.Length == 0) return;
            if (_openDropdownKey == setting.Key)
                CloseDropdown();
            else
            {
                _openDropdownKey = setting.Key;
                _dropdownHoverIndex = Array.IndexOf(setting.Values, _iniValues.GetValueOrDefault(setting.Key, setting.Values[0]));
                if (_dropdownHoverIndex < 0) _dropdownHoverIndex = 0;
            }
            Invalidate();
        }

        void CloseDropdown()
        {
            _openDropdownKey = null;
            _dropdownHoverIndex = -1;
            Invalidate();
        }

        void ApplyDropdownSelection()
        {
            var setting = GetVisibleSetting(_sel);
            if (setting == null || _dropdownHoverIndex < 0) return;
            string val = setting.Values[_dropdownHoverIndex];
            _iniValues[setting.Key] = val;
            WriteSetting(setting.Key, val);
            CloseDropdown();
            Invalidate();
        }

        // ---------------------------------------------------------------------------
        // Setup Modding - installs only the pieces every mod needs to load:
        // Skyline (the plugin loader) and ARCropolis (the mod manager).
        async Task RunSetupModdingAsync()
        {
            if (_setupModding) return;
            _setupModding = true;
            _setupProgress = 0;
            _setupLog = "";
            _setupStep = "Starting...";
            Invalidate();

            try
            {
                string arcDir = Path.Combine(_repo, "sd", "ultimate", "arcropolis");

                _setupLog += "[1/3] Preparing folders\n";
                Directory.CreateDirectory(_modsDir);
                Directory.CreateDirectory(_skylineDir);
                Directory.CreateDirectory(arcDir);
                Directory.CreateDirectory(_pluginsDir);
                _setupProgress = 10; Invalidate();

                // Skyline - the plugin loader itself.
                if (File.Exists(Path.Combine(_skylineDir, "exefs", "subsdk9")))
                {
                    _setupLog += "[2/3] Skyline already installed\n";
                    _setupProgress = 45;
                }
                else
                {
                    var skyline = Pinned("skyline");
                    _setupStep = $"Downloading Skyline {skyline.Version}...";
                    _setupLog += "[2/3] Installing Skyline\n";
                    _setupLog += $"      Skyline {skyline.Version}\n";
                    await DownloadPinnedAndExtract(skyline, _skylineDir);

                    int plugins = ImportPlugins(_skylineDir);
                    if (plugins > 0) _setupLog += $"      imported {plugins} plugin(s)\n";
                    _setupProgress = 45;
                }
                Invalidate();

                // ARCropolis - the mod manager that every mod goes through.
                if (File.Exists(Path.Combine(_pluginsDir, "libarcropolis.nro")))
                {
                    _setupLog += "[3/3] ARCropolis already installed\n";
                    _setupProgress = 85;
                }
                else
                {
                    var arcropolis = Pinned("arcropolis");
                    _setupStep = $"Downloading ARCropolis {arcropolis.Version}...";
                    _setupLog += "[3/3] Installing ARCropolis\n";
                    _setupLog += $"      ARCropolis {arcropolis.Version}\n";
                    await DownloadPinnedAndExtract(arcropolis, arcDir);

                    // ARCropolis ships as a Skyline plugin; it has to sit in the
                    // plugin folder (the layered-fs patch) to be loaded at boot.
                    _setupLog += $"      imported {ImportPlugins(arcDir)} plugin(s)\n";
                    _setupProgress = 85;
                }
                Invalidate();

                // Config files, only when missing - never overwrite a user's setup.
                string skylineConfig = Path.Combine(_skylineDir, "config.ini");
                if (!File.Exists(skylineConfig))
                    await File.WriteAllTextAsync(skylineConfig, "[skyline]\nlog_level = info\n");
                string arcConfig = Path.Combine(arcDir, "config.ini");
                if (!File.Exists(arcConfig))
                    await File.WriteAllTextAsync(arcConfig, "[arcropolis]\nmount_sd = true\n");

                // Releases unpack differently between versions; if a loader landed
                // somewhere else under sd:/, move it where the runtime looks for it.
                EnsureLoaderFile("subsdk9", Path.Combine(_skylineDir, "exefs", "subsdk9"));
                EnsureLoaderFile("libarcropolis.nro", Path.Combine(_pluginsDir, "libarcropolis.nro"));

                _setupStep = "Verifying installation...";
                _setupLog += ModdingInstalled
                    ? "[done] Modding stack ready\n"
                    : "[done] Finished, but the plugin folder still looks incomplete\n";
                _setupProgress = 100;
            }
            catch (Exception ex)
            {
                _setupLog += $"\nERROR: {ex.Message}";
                _setupStep = "Failed: " + ex.Message;
            }
            finally
            {
                _setupModding = false;
                UpdateMods();
                // The marketplace caches the installed plugin/loader list.
                _ = PushInstalledState();
                Invalidate();
            }
        }

        // Releases unpack differently between versions; if a loader landed somewhere
        // else under sd:/, copy it to the path the runtime actually loads.
        void EnsureLoaderFile(string fileName, string targetPath)
        {
            try
            {
                if (File.Exists(targetPath)) return;

                string sdRoot = Path.Combine(_repo, "sd");
                if (!Directory.Exists(sdRoot)) return;

                string? found = Directory
                    .GetFiles(sdRoot, fileName, SearchOption.AllDirectories)
                    .FirstOrDefault();

                if (found == null) return;

                string? dir = Path.GetDirectoryName(targetPath);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.Copy(found, targetPath, true);
            }
            catch { }
        }

        // Copies any Skyline plugins found under a freshly extracted release into
        // the layered-fs plugin folder the runtime actually loads from.
        int ImportPlugins(string root)
        {
            if (!Directory.Exists(root)) return 0;
            int imported = 0;
            try
            {
                Directory.CreateDirectory(_pluginsDir);
                foreach (string file in Directory.GetFiles(root, "*.nro", SearchOption.AllDirectories))
                {
                    string target = Path.Combine(_pluginsDir, Path.GetFileName(file));
                    if (File.Exists(target) && new FileInfo(target).Length == new FileInfo(file).Length) continue;
                    File.Copy(file, target, true);
                    imported++;
                }
            }
            catch (Exception ex)
            {
                _setupLog += $"      plugin import failed: {ex.Message}\n";
            }
            return imported;
        }

        // Scan for known Switch emulators and their SD card locations.
        List<EmulatorInfo> FindEmulators()
        {
            var emus = new List<EmulatorInfo>();
            var roots = new[] { Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) };
            var known = new (string Name, string Folder)[]
            {
                ("Ryujinx", "Ryujinx"),
                ("yuzu", "yuzu"),
                ("Sudachi", "sudachi"),
                ("Suyu", "suyu"),
                ("Eden", "eden"),
                ("Citron", "citron"),
                ("Torzu", "torzu"),
            };
            foreach (string root in roots)
            {
                if (!Directory.Exists(root)) continue;
                foreach (var (name, folder) in known)
                {
                    string basePath = Path.Combine(root, folder);
                    if (!Directory.Exists(basePath)) continue;
                    // Common SD card locations
                    string[] sdCandidates = { "sdcard", "sdmc", "portable" };
                    foreach (string sd in sdCandidates)
                    {
                        string sdPath = Path.Combine(basePath, sd);
                        if (!Directory.Exists(sdPath)) continue;
                        string pluginsPath = Path.Combine(sdPath, "atmosphere", "contents", "01006A800016E000", "romfs", "skyline", "plugins");
                        string modsPath = Path.Combine(sdPath, "ultimate", "mods");
                        int pluginCount = Directory.Exists(pluginsPath) ? Directory.GetFiles(pluginsPath, "*.nro").Length : 0;
                        int modCount = Directory.Exists(modsPath) ? Directory.GetDirectories(modsPath).Length : 0;
                        if (pluginCount > 0 || modCount > 0)
                        {
                            emus.Add(new EmulatorInfo
                            {
                                Name = name,
                                Path = basePath,
                                SdPath = sdPath,
                                ModCount = modCount,
                                PluginCount = pluginCount
                            });
                        }
                    }
                }
            }
            return emus;
        }

        async Task ImportFromEmulator(EmulatorInfo emu)
        {
            _setupMode = "import";
            _setupModding = true;
            _setupProgress = 0;
            _setupLog = "";
            _setupStep = "Importing from " + emu.Name + "...";
            Invalidate();

            try
            {
                // Copy plugins
                string srcPlugins = Path.Combine(emu.SdPath, "atmosphere", "contents", "01006A800016E000", "romfs", "skyline", "plugins");
                if (Directory.Exists(srcPlugins))
                {
                    _setupLog += $"[1/3] Copying {Directory.GetFiles(srcPlugins, "*.nro").Length} plugins...\n";
                    int imported = ImportPlugins(srcPlugins);
                    _setupLog += $"      imported {imported} plugin(s)\n";
                }
                _setupProgress = 40; Invalidate();

                // Copy mods
                string srcMods = Path.Combine(emu.SdPath, "ultimate", "mods");
                if (Directory.Exists(srcMods))
                {
                    _setupLog += $"[2/3] Copying {Directory.GetDirectories(srcMods).Length} mods...\n";
                    Directory.CreateDirectory(_modsDir);
                    foreach (string modDir in Directory.GetDirectories(srcMods))
                    {
                        string name = Path.GetFileName(modDir);
                        string dst = Path.Combine(_modsDir, name);
                        if (!Directory.Exists(dst))
                        {
                            CopyDirectory(modDir, dst);
                            _setupLog += $"      {name}\n";
                        }
                    }
                }
                _setupProgress = 80; Invalidate();

                // Copy config if exists
                string srcConfig = Path.Combine(emu.SdPath, "ultimate", "skyline", "config.ini");
                string dstConfig = Path.Combine(_skylineDir, "config.ini");
                if (File.Exists(srcConfig) && !File.Exists(dstConfig))
                {
                    Directory.CreateDirectory(_skylineDir);
                    File.Copy(srcConfig, dstConfig);
                    _setupLog += "[3/3] Copied skyline config\n";
                }

                _setupLog += "[done] Import complete\n";
                _setupProgress = 100;
            }
            catch (Exception ex)
            {
                _setupLog += $"\nERROR: {ex.Message}";
                _setupStep = "Failed: " + ex.Message;
            }
            finally
            {
                _setupModding = false;
                UpdateMods();
                _ = PushInstalledState();
                Invalidate();
            }
        }

        // Every binary the launcher downloads by itself (the loader, the mod manager and
        // the community plugins mods list as requirements) is pinned here: one release,
        // one URL, its SHA-256 and a size cap. Nothing is resolved from "latest" or from
        // a URL the mod page supplies. The hashes are GitHub's recorded asset digests;
        // for the .nro plugins they match tools/std_plugins.py (NRO Hook's release has
        // no GitHub digest, so its hash is the one std_plugins.py recorded). To move to
        // a newer release, check it runs, then update its row.
        sealed record PinnedDownload(string Key, string Name, string Repo, string Version, string Url, string Sha256, long MaxBytes);

        static readonly PinnedDownload[] PinnedDownloads =
        {
            // skyline-dev/skyline publishes only the "beta" tag; the hash pins the asset
            // published 2026-04-12, so a re-published "beta" is refused, not installed.
            new("skyline", "Skyline", "skyline-dev/skyline", "beta (2026-04-12)",
                "https://github.com/skyline-dev/skyline/releases/download/beta/skyline.zip",
                "d677c5bbbfbf55674b047c8d5d11f8a4d71b6a42fbc7ee4bf70f462b077c4ab0", 1L << 20),
            new("arcropolis", "ARCropolis", "Raytwo/ARCropolis", "4.1.1",
                "https://github.com/Raytwo/ARCropolis/releases/download/v4.1.1/release.zip",
                "08b98371f069bc54a326660bc828288c05dd9a4ac0e3c7c00aca557796e032c6", 16L << 20),
            new("smashline", "Smashline", "HDR-Development/smashline", "1.6.8",
                "https://github.com/HDR-Development/smashline/releases/download/v1.6.8/libsmashline_plugin.nro",
                "a83636e565be0729271aa7c21817da9aa3b8b3e9b3abed7499c1eeae4cb57d5c", 8L << 20),
            new("nro_hook", "NRO Hook", "ultimate-research/nro-hook-plugin", "0.4.0",
                "https://github.com/ultimate-research/nro-hook-plugin/releases/download/v0.4.0/libnro_hook.nro",
                "ca7aec03aee49fea539dcb92569fea5bc5389f77377ba2a30e4d0b7ebd0255e6", 8L << 20),
        };

        // Plugins the runtime provides itself; the release copy is not installed
        // (tools/std_plugins.py: lib_paramconfig's main hangs here).
        static readonly string[] RuntimeProvidedPluginRepos = { "CSharpM7/lib_paramconfig" };

        static PinnedDownload Pinned(string key) =>
            PinnedDownloads.First(p => p.Key == key);

        static PinnedDownload? PinnedForRepo(string repo) =>
            PinnedDownloads.FirstOrDefault(p => p.Repo.Equals(repo, StringComparison.OrdinalIgnoreCase));

        // Downloads a pinned release (never more than its size cap), checks its SHA-256,
        // and only then writes the .nro or extracts the zip into destDir.
        async Task DownloadPinnedAndExtract(PinnedDownload pin, string destDir)
        {
            if (string.IsNullOrWhiteSpace(pin.Sha256))
                throw new InvalidOperationException($"{pin.Name} {pin.Version} has no SHA-256 recorded yet; refusing to install it.");

            byte[] bytes;
            using (var request = new HttpRequestMessage(HttpMethod.Get, pin.Url))
            {
                request.Headers.Add("User-Agent", "OpenSmash-Launcher/1.0");
                using var response = await _downloadClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
                response.EnsureSuccessStatusCode();
                if (response.Content.Headers.ContentLength is long declared && declared > pin.MaxBytes)
                    throw new InvalidDataException($"{pin.Name} download is {declared} bytes, over its {pin.MaxBytes}-byte cap.");

                using var stream = await response.Content.ReadAsStreamAsync();
                using var buffer = new MemoryStream();
                var chunk = new byte[81920];
                int read;
                while ((read = await stream.ReadAsync(chunk, 0, chunk.Length)) > 0)
                {
                    if (buffer.Length + read > pin.MaxBytes)
                        throw new InvalidDataException($"{pin.Name} download is over its {pin.MaxBytes}-byte cap.");
                    buffer.Write(chunk, 0, read);
                }
                bytes = buffer.ToArray();
            }

            string got = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes));
            if (!got.Equals(pin.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(
                    $"{pin.Name} {pin.Version} is not the release checked here (SHA-256 {got[..16].ToLowerInvariant()}..., expected {pin.Sha256[..16]}...); not installed.");

            Directory.CreateDirectory(destDir);
            string fileName = Path.GetFileName(new Uri(pin.Url).AbsolutePath);

            // Some releases publish the .nro directly instead of an archive.
            if (fileName.EndsWith(".nro", StringComparison.OrdinalIgnoreCase))
            {
                await File.WriteAllBytesAsync(Path.Combine(destDir, fileName), bytes);
                return;
            }

            string zipPath = Path.Combine(Path.GetTempPath(), $"setup_{Guid.NewGuid():N}.zip");
            await File.WriteAllBytesAsync(zipPath, bytes);
            try
            {
                System.IO.Compression.ZipFile.ExtractToDirectory(zipPath, destDir, true);
            }
            finally
            {
                File.Delete(zipPath);
            }
        }

        // ---------------------------------------------------------------------------
        // Mod actions
        async void AddMod()
        {
            if (_modsBusy) return;
            if (!FolderPicker.Show(this, out string src, out bool asLink)) return;
            string name = Path.GetFileName(src.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            string dst = Path.Combine(_modsDir, name);
            if (Directory.Exists(dst) || File.Exists(dst))
            {
                MessageBox.Show($"A mod called \"{name}\" is already installed.", "OpenSmash");
                return;
            }
            _modsBusy = true;
            Invalidate();
            try
            {
                Directory.CreateDirectory(_modsDir);
                if (asLink) Directory.CreateSymbolicLink(dst, src);
                else CopyDirectory(src, dst);
            }
            catch (Exception ex)
            {
                _modsBusy = false;
                Invalidate();
                MessageBox.Show(asLink ? $"Could not add the link: {ex.Message}" : $"Could not copy: {ex.Message}", "OpenSmash-Ultimate");
                return;
            }
            _pluginBuildUi = true;
            _pluginFrac = -1;
            _pluginDetail = "Building the plugin";
            _pluginEta = "";
            Invalidate();
            string why = "";
            try
            {
                await Task.Run(() => OsMods(new[] { "enable", name }, line =>
                {
                    if (!line.StartsWith("LOG ")) return;
                    string text = line.Substring(4).Trim();
                    BeginInvoke(() => NotePluginBuild(text));
                }));
                RememberNewMods(new[] { name });
            }
            catch (Exception ex) { why = ex.Message; }
            _modsBusy = false;
            _pluginBuildUi = false;
            UpdateMods();
            await PushInstalledState();
            PlayDone();
            Invalidate();
            if (why.Length > 0) MessageBox.Show(why, "OpenSmash");
        }

        void RemoveMod()
        {
            // _sel indexes the whole mods view (action tiles first), so map it back.
            int index = _sel - ModActionCount;
            if (index < 0 || index >= _modLines.Count || _modsBusy) return;
            var line = _modLines[index];
            string label = line.Header || line.Pick ? line.Title + (line.Pick ? " and its colors" : " and its parts") : line.Title;
            var r = MessageBox.Show($"Delete \"{label}\"?", "OpenSmash", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
            if (r != DialogResult.Yes) return;
            var args = new List<string> { "remove" };
            args.AddRange(line.Names);
            _modsBusy = true;
            SetLoad("REMOVING MOD", -1);
            Invalidate();
            _ = Task.Run(() =>
            {
                string why = "";
                try { OsMods(args.ToArray()); }
                catch (Exception ex) { why = ex.Message; }
                BeginInvoke(() =>
                {
                    _modsBusy = false;
                    ClearLoad();
                    UpdateMods();
                    _ = PushInstalledState();
                    Invalidate();
                    if (why.Length > 0) MessageBox.Show($"Could not remove: {why}", "OpenSmash");
                });
            });
        }

        // Conflicts, updates and shared lists (tools/os_mods.py; TRLLZR's mod manager ideas, NOTICE.md).
        static JsonElement? OsModsResult(string output)
        {
            string line = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                .LastOrDefault(l => l.StartsWith("RESULT ")) ?? "";
            if (line.Length == 0) return null;
            try { using var doc = JsonDocument.Parse(line.Substring("RESULT ".Length)); return doc.RootElement.Clone(); }
            catch { return null; }
        }

        async Task CheckMods()
        {
            if (_modsBusy) return;
            _modsBusy = true;
            SetLoad("CHECKING MODS", -1);
            Invalidate();
            string conflictsOut = "", updatesOut = "", why = "";
            await Task.Run(() =>
            {
                try
                {
                    RunPython(new[] { Path.Combine(_repo, "tools", "os_mods.py"), "conflicts" }, out conflictsOut);
                    RunPython(new[] { Path.Combine(_repo, "tools", "os_mods.py"), "updates" }, out updatesOut);
                }
                catch (Exception ex) { why = ex.Message; }
            });
            _modsBusy = false;
            ClearLoad();
            Invalidate();
            if (why.Length > 0) { MessageBox.Show($"Could not check the mods: {why}", "OpenSmash"); return; }
            var text = new System.Text.StringBuilder();
            var c = OsModsResult(conflictsOut);
            if (c is JsonElement cr && cr.TryGetProperty("conflicts", out var list) && list.ValueKind == JsonValueKind.Array)
            {
                int nc = list.GetArrayLength();
                text.AppendLine(nc == 0 ? "No file is changed by two turned-on mods."
                                        : $"{nc} file(s) changed by more than one turned-on mod (the game reads the last one's):");
                int shown = 0;
                foreach (var x in list.EnumerateArray())
                {
                    if (shown++ == 12) { text.AppendLine($"  ... and {nc - 12} more"); break; }
                    var ms = x.GetProperty("mods").EnumerateArray().Select(m => m.GetString()).ToArray();
                    text.AppendLine($"  {GetString(x, "path")}: {string.Join(", ", ms)} -> {GetString(x, "wins")}");
                }
            }
            else text.AppendLine("Conflicts: could not be read.");
            text.AppendLine();
            var u = OsModsResult(updatesOut);
            var updates = new List<(string gb, string file, string title)>();
            if (u is JsonElement ur && ur.TryGetProperty("updates", out var ul) && ul.ValueKind == JsonValueKind.Array)
            {
                foreach (var x in ul.EnumerateArray())
                    updates.Add((GetString(x, "gamebanana"), GetString(x, "file_id"), GetString(x, "title")));
                text.AppendLine(updates.Count == 0 ? "Every GameBanana mod is up to date." : $"{updates.Count} mod(s) with a newer file on GameBanana:");
                foreach (var x in updates) text.AppendLine($"  {x.title}");
            }
            else text.AppendLine("Updates: GameBanana could not be reached.");
            _checkModsText = text.ToString().Trim();
            _checkModsUpdates = updates;
            _checkModsPrompt = true;
            PlayPopup();
            Invalidate();
        }

        async Task InstallModUpdates()
        {
            if (_modsBusy || _checkModsUpdates.Count == 0) return;
            var updates = _checkModsUpdates;
            _checkModsPrompt = false;
            _modsBusy = true;
            Invalidate();
            string failed = "";
            foreach (var x in updates)
            {
                SetLoad("UPDATING " + x.title.ToUpperInvariant(), -1);
                try { await Task.Run(() => OsMods(new[] { "install", x.gb, x.file })); }
                catch (Exception ex) { failed += $"\n{x.title}: {ex.Message}"; }
            }
            _modsBusy = false;
            ClearLoad();
            UpdateMods();
            _ = PushInstalledState();
            Invalidate();
            SetLoad(failed.Length == 0 ? "CHECK DONE" : "SOME FAILED", 1);
            _ = Task.Delay(1800).ContinueWith(_ => BeginInvoke(ClearLoad));
            PlayDone();
        }

        void ShareModList(bool export)
        {
            if (_modsBusy) return;
            string path;
            if (export)
            {
                using var d = new SaveFileDialog { Filter = "Mod list (*.json)|*.json", FileName = "opensmash-mods.json" };
                if (d.ShowDialog(this) != DialogResult.OK) return;
                path = d.FileName;
            }
            else
            {
                using var d = new OpenFileDialog { Filter = "Mod list (*.json)|*.json|All files|*.*" };
                if (d.ShowDialog(this) != DialogResult.OK) return;
                path = d.FileName;
            }
            _modsBusy = true;
            SetLoad(export ? "SAVING THE LIST" : "INSTALLING THE LIST", -1);
            Invalidate();
            _ = Task.Run(() =>
            {
                string why = "", output = "";
                try
                {
                    RunPython(new[] { Path.Combine(_repo, "tools", "os_mods.py"), export ? "export-list" : "import-list", path }, out output);
                }
                catch (Exception ex) { why = ex.Message; }
                BeginInvoke(() =>
                {
                    _modsBusy = false;
                    ClearLoad();
                    UpdateMods();
                    _ = PushInstalledState();
                    Invalidate();
                    var r = OsModsResult(output);
                    if (why.Length == 0 && r is JsonElement e && e.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.False)
                        why = GetString(e, "error");
                    if (why.Length > 0) { MessageBox.Show($"Could not {(export ? "save" : "install")} the list: {why}", "OpenSmash"); return; }
                    if (export) { MessageBox.Show($"Saved: {path}", "OpenSmash - mod list"); return; }
                    int Count(string key) => r is JsonElement j && j.TryGetProperty(key, out var a) && a.ValueKind == JsonValueKind.Array ? a.GetArrayLength() : 0;
                    string missing = r is JsonElement j2 && j2.TryGetProperty("missing", out var m) && m.ValueKind == JsonValueKind.Array
                        ? string.Join(", ", m.EnumerateArray().Select(x => x.GetString())) : "";
                    MessageBox.Show($"{Count("installed")} installed, {Count("already")} already here" +
                                    (missing.Length > 0 ? $", not on GameBanana (ask your friend for them): {missing}" : "."), "OpenSmash - mod list");
                });
            });
        }

        void CopyDirectory(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (string file in Directory.GetFiles(src))
                File.Copy(file, Path.Combine(dst, Path.GetFileName(file)), true);
            foreach (string dir in Directory.GetDirectories(src))
                CopyDirectory(dir, Path.Combine(dst, Path.GetFileName(dir)));
        }

        static void PlayPopup() { try { System.Media.SystemSounds.Asterisk.Play(); } catch { } }
        static void PlayDone() { try { System.Media.SystemSounds.Exclamation.Play(); } catch { } }

        // ---------------------------------------------------------------------------
        // Play
        void SetLoad(string label, float progress)
        {
            if (InvokeRequired) { BeginInvoke(() => SetLoad(label, progress)); return; }
            _loadOn = true;
            _loadLabel = label;
            _loadProgress = progress;
        }

        void ClearLoad()
        {
            if (InvokeRequired) { BeginInvoke(ClearLoad); return; }
            _loadOn = false;
        }

        void NotePluginBuild(string text)
        {
            var step = Regex.Match(text, @"^(.*?)(\d+)\s*%\s*,?\s*(.*)$");
            if (step.Success && int.TryParse(step.Groups[2].Value, out int pct))
            {
                string detail = Regex.Replace(step.Groups[1].Value, @"[^A-Za-z0-9.)\]]+$", "").Trim();
                // The percent is the whole build's (tools\skyline.py run_logged), across every program it runs;
                // one that goes down (an older tools folder: a program's own bar starting over) is not shown.
                float frac = Math.Clamp(pct / 100f, 0f, 1f);
                if (frac >= _pluginFrac)
                {
                    _pluginDetail = detail.Length > 0 ? detail : text;
                    _pluginFrac = frac;
                    _pluginEta = step.Groups[3].Value.Trim().TrimStart(',', ' ');
                }
            }
            else if (!text.Contains("recomp.py") && text.Length < 240)
                _pluginDetail = text;
            Invalidate();
        }

        async Task<bool> PrepareModsForLaunch()
        {
            try
            {
                if (_launchMode == "vanilla")
                {
                    SetLoad("PLAY WITHOUT MODS", -1);
                    await Task.Run(() => OsMods(new[] { "vanilla" }));
                    _modsModded = false;
                }
                else if (!_modsModded && _mods.Any(m => m.Enabled))
                {
                    SetLoad("APPLYING MODS", -1);
                    await Task.Run(() => OsMods(new[] { "apply" }));
                    _modsModded = true;
                }
                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "OpenSmash");
                return false;
            }
            finally { ClearLoad(); }
        }

        void ToggleMod(string[] names)
        {
            if (_modsBusy || names == null || names.Length == 0) return;
            bool allOn = names.All(n => _mods.FirstOrDefault(x => x.Name == n)?.Enabled == true);
            string cmd = allOn ? "disable" : "enable";
            SetLoad(allOn ? "TURNING THE MOD OFF" : "TURNING THE MOD ON", -1);
            var args = new List<string> { cmd };
            args.AddRange(names);
            RunModEnable(args.ToArray(), allOn);
        }

        void TogglePickMod(ModLine line)
        {
            if (_modsBusy || line.Names == null || line.Names.Length == 0) return;
            if (line.Enabled)
            {
                SetLoad("TURNING THE MOD OFF", -1);
                var args = new List<string> { "disable" };
                args.AddRange(line.Names);
                RunModEnable(args.ToArray(), true);
            }
            else
            {
                int i = Math.Clamp(line.Active, 0, line.Names.Length - 1);
                SetLoad("TURNING THE MOD ON", -1);
                RunModEnable(new[] { "enable", line.Names[i] }, false);
            }
        }

        void SelectPickVariant(ModLine line, int index)
        {
            if (_modsBusy || line.Names == null || index < 0 || index >= line.Names.Length) return;
            _openDropdownKey = null;
            if (line.Enabled && line.Active == index) return;
            SetLoad("SWITCHING COLOR", -1);
            RunModEnable(new[] { "enable", line.Names[index] }, false);
        }

        void RunModEnable(string[] args, bool turningOff)
        {
            _modsBusy = true;
            _ = Task.Run(() =>
            {
                string why = "";
                try { OsMods(args); }
                catch (Exception ex) { why = ex.Message; }
                BeginInvoke(() =>
                {
                    _modsBusy = false;
                    ClearLoad();
                    UpdateMods();
                    Invalidate();
                    if (why.Length > 0) MessageBox.Show(why, "OpenSmash");
                });
            });
        }

        async Task RefreshPlugins()
        {
            if (_modsBusy) return;
            _modsBusy = true;
            _pluginBuildUi = true;
            _pluginFrac = -1;
            _pluginDetail = "Refreshing the plugin";
            _pluginEta = "";
            Invalidate();
            string why = "";
            try
            {
                await Task.Run(() => OsMods(new[] { "refresh" }, line =>
                {
                    if (!line.StartsWith("LOG ")) return;
                    string text = line.Substring(4).Trim();
                    BeginInvoke(() => NotePluginBuild(text));
                }));
            }
            catch (Exception ex) { why = ex.Message; }
            _modsBusy = false;
            _pluginBuildUi = false;
            UpdateMods();
            PlayDone();
            Invalidate();
            if (why.Length > 0) MessageBox.Show(why, "OpenSmash");
        }

        async Task BuildPlugins()
        {
            if (_modsBusy) return;
            _modsBusy = true;
            _pluginBuildUi = true;
            _pluginFrac = -1;
            _pluginDetail = "Starting the plugin build";
            _pluginEta = "";
            Invalidate();
            string why = "";
            try
            {
                await Task.Run(() => OsMods(new[] { "build" }, line =>
                {
                    if (!line.StartsWith("LOG ")) return;
                    string text = line.Substring(4).Trim();
                    BeginInvoke(() => NotePluginBuild(text));
                }));
            }
            catch (Exception ex) { why = ex.Message; }
            _modsBusy = false;
            _pluginBuildUi = false;
            UpdateMods();
            PlayDone();
            Invalidate();
            if (why.Length > 0) MessageBox.Show(why, "OpenSmash");
        }

        async Task FetchPlugins(bool again)
        {
            if (_setupModding) return;
            _setupModding = true;
            _setupLog = "";
            _setupStep = again ? "Downloading the community plugins again" : "Downloading missing community plugins";
            Invalidate();
            string why = "";
            try
            {
                await Task.Run(() => OsMods(again ? new[] { "fetch", "--again" } : new[] { "fetch" }, line =>
                {
                    if (!line.StartsWith("LOG ")) return;
                    BeginInvoke(() => { _setupLog += line.Substring(4) + "\n"; _setupProgress = Math.Min(95, _setupProgress + 10); Invalidate(); });
                }));
                _setupProgress = 100;
                _setupStep = "Community plugins are ready";
            }
            catch (Exception ex) { why = ex.Message; _setupStep = "Download stopped"; }
            _setupModding = false;
            UpdateMods();
            PlayDone();
            Invalidate();
            if (why.Length > 0) MessageBox.Show(why, "OpenSmash");
        }

        async Task<string> UnbuiltPluginChoice()
        {
            // Without mods it is the same program (build\ssbu.exe, the community plugins loaded): a link it needs
            // (an update changed the runtime) is asked for as with mods. The mods' own plugins are not loaded then,
            // so one waiting, to be refreshed or not linked in yet is not (needs_build_vanilla).
            bool vanilla = _launchMode == "vanilla";
            await RefreshMods();
            var waiting = vanilla ? new List<string>() : _mods.Where(m => m.Enabled && m.Code == "waiting")
                .Select(m => string.IsNullOrEmpty(m.Title) ? m.Name : m.Title)
                .Distinct()
                .ToList();
            string reason = vanilla ? _vanillaBuildReason : _pluginBuildReason;
            bool full = !string.IsNullOrEmpty(reason);
            if (vanilla && !full) return "play";
            string why = full ? PluginUpdateWhy(waiting, reason) : PluginRefreshWhy(waiting, _pluginRefreshReason);
            if (why.Length == 0) return "play";
            _pluginPromptKind = full ? "update" : "refresh";
            _pluginPromptText = why;
            _pluginPrompt = true;
            PlayPopup();
            _pluginPromptWait = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
            Invalidate();
            string choice = await _pluginPromptWait.Task;
            _pluginPrompt = false;
            Invalidate();
            return choice;
        }

        static string PluginRefreshWhy(List<string> waiting, string reason)
        {
            if (waiting.Count == 1)
                return waiting[0] + " has a new plugin file. Refreshing rebuilds its DLL. The game program stays as it is.";
            if (waiting.Count > 1)
                return "These mods have new plugin files. Refreshing rebuilds their DLLs. The game program stays as it is.\n\n" +
                    string.Join("\n", waiting.Take(8));
            if (!string.IsNullOrEmpty(reason))
                return "A moveset plugin has to be turned into a DLL again. The game program stays as it is.";
            return "";
        }

        static string PluginUpdateWhy(List<string> waiting, string reason)
        {
            var parts = new List<string>();
            if (waiting.Count == 1)
                parts.Add(waiting[0] + " has a plugin that isn't ready yet. A match loads the vanilla fighter until it is updated.");
            else if (waiting.Count > 1)
                parts.Add("These mods have plugins that aren't ready yet. A match loads the vanilla fighter until they are updated.\n\n" +
                    string.Join("\n", waiting.Take(8)));
            bool sameAsWaiting = reason.Contains("isn't translated") || reason.Contains("isn't recompiled");
            if (!string.IsNullOrEmpty(reason) && !(waiting.Count > 0 && sameAsWaiting))
            {
                if (reason.Contains("hooks a function"))
                    parts.Add("A mod hooks the middle of a function. The game has to be updated once so that hook can run.");
                else if (reason.StartsWith("the plugin "))
                    parts.Add(reason.Substring("the plugin ".Length).Replace(" isn't in the game", "") + " was added. The game has to be updated once so that plugin can run.");
                else if (reason.Contains("isn't in the game") || reason.Contains("isn't fetched"))
                    parts.Add("A community plugin the mods rely on isn't in the game yet.");
                else if (reason.Contains("patches.txt"))
                    parts.Add("A mod patches the game's code. The game has to be updated once so that patch is in it.");
                else if (reason.Contains("newer than"))
                    parts.Add("The game itself changed. It has to be linked once so Play uses the current code.");
                else if (sameAsWaiting)
                    parts.Add("A moveset's plugin still has to be turned into a DLL.");
                else if (reason.Contains("isn't built"))
                    parts.Add("The game isn't built yet.");
                else
                    parts.Add(reason);
            }
            return string.Join("\n\n", parts);
        }

        void AskShadersIfNeeded()
        {
            if (!_built || ShadersDone()) return;
            _shaderPrompt = true;
            _shaderPromptAt = Now;
            Invalidate();
        }

        bool ShadersDone()
        {
            string marker = Path.Combine(_repo, "dump", "shadercache", "complete.txt");
            if (!File.Exists(marker)) return false;
            var ver = Regex.Match(File.ReadAllText(Path.Combine(_repo, "runtime", "gpu_shader_cache.h")), @"#define SHADER_CACHE_VERSION (\d+)");
            if (!ver.Success) return false;
            return File.ReadAllText(marker).Contains("version " + ver.Groups[1].Value);
        }

        void StartShaders(string priority)
        {
            _shaderPrompt = false;
            if (_shaderProc != null && !_shaderProc.HasExited) return;
            _shaderUi = true;
            _shaderFrac = 0;
            _shaderDetail = "Listing shader files";
            _shaderEta = "";
            var psi = new ProcessStartInfo
            {
                FileName = _pythonExe,
                WorkingDirectory = _repo,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            psi.ArgumentList.Add(Path.Combine(_repo, "tools", "shader_aot.py"));
            psi.ArgumentList.Add("--priority");
            psi.ArgumentList.Add(priority);
            var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) try { BeginInvoke(() => NoteShaderLine(e.Data)); } catch (InvalidOperationException) { } };
            proc.ErrorDataReceived += (_, e) => { if (e.Data != null) try { BeginInvoke(() => NoteShaderLine(e.Data)); } catch (InvalidOperationException) { } };
            proc.Exited += (_, _) =>
            {
                try { BeginInvoke(() => { _shaderUi = false; Invalidate(); }); } catch (InvalidOperationException) { }
            };
            try
            {
                proc.Start();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
                _shaderProc = proc;
            }
            catch (Exception ex)
            {
                _shaderUi = true;
                _shaderFrac = 0;
                _shaderDetail = "Could not start the shader compile: " + ex.Message;
            }
            Invalidate();
        }

        void NoteShaderLine(string line)
        {
            var files = Regex.Match(line, @"(listing|reading) shader files:\s*([\d,]+)\s+of\s+([\d,]+)", RegexOptions.IgnoreCase);
            if (files.Success
                && double.TryParse(files.Groups[2].Value.Replace(",", ""), out double read)
                && double.TryParse(files.Groups[3].Value.Replace(",", ""), out double filesTotal)
                && filesTotal > 0)
            {
                _shaderFrac = (float)(read / filesTotal);
                string phase = files.Groups[1].Value.Equals("listing", StringComparison.OrdinalIgnoreCase)
                    ? "Listing shader files" : "Reading shader files";
                _shaderDetail = $"{phase}  {read:N0} / {filesTotal:N0}";
                _shaderEta = "";
                Invalidate();
                return;
            }
            var count = Regex.Match(line, @"([\d,]+)\s+of\s+([\d,]+)\s+shaders(?:,\s*(.+))?$", RegexOptions.IgnoreCase);
            if (count.Success
                && double.TryParse(count.Groups[1].Value.Replace(",", ""), out double done)
                && double.TryParse(count.Groups[2].Value.Replace(",", ""), out double total)
                && total > 0)
            {
                _shaderFrac = (float)(done / total);
                _shaderDetail = $"Compiling shaders  {done:N0} / {total:N0}";
                _shaderEta = count.Groups[3].Success ? count.Groups[3].Value.Trim() : "";
            }
            else if (line.StartsWith("getting ready", StringComparison.OrdinalIgnoreCase))
            {
                _shaderFrac = 0;
                _shaderDetail = "Translating vertex programs";
                _shaderEta = "";
            }
            else if (line.StartsWith("compiling with", StringComparison.OrdinalIgnoreCase))
            {
                _shaderFrac = 0;
                _shaderDetail = "Compiling shaders";
                _shaderEta = "";
            }
            Invalidate();
        }

        async void StartPlay()
        {
            if (Busy) return;
            _startingPlay = true;
            Invalidate();
            try { await StartPlayCore(); }
            finally
            {
                _startingPlay = false;
                Invalidate();
            }
        }

        async Task StartPlayCore()
        {
            if (_prepInflight != null && !_prepInflight.IsCompleted)
            {
                if (_playQueued) return;
                _playQueued = true;
                bool ready = await _prepInflight;
                _playQueued = false;
                if (!ready) return;
            }
            else if (_loadOn) return;
            if (!_built)
            {
                MessageBox.Show("The game is not built yet. Run the installer first.", "OpenSmash");
                return;
            }
            string unbuilt = await UnbuiltPluginChoice();
            if (unbuilt == "cancel") return;
            if (unbuilt == "build")
            {
                await BuildPlugins();
                return;
            }
            if (unbuilt == "refresh")
            {
                await RefreshPlugins();
                return;
            }
            if (!await PrepareModsForLaunch()) return;
            try
            {
                if (!SaveGameSettings()) return;
            }
            catch (Exception ex)
            {
                MessageBox.Show("Could not save the settings.\n\n" + ex.Message, "OpenSmash");
                return;
            }

            bool shown = false;
            if (_gameProcess != null && !_gameProcess.HasExited)
            {
                // Game already running in background - bring to front
                for (int i = 0; i < 20 && !shown; i++)
                {
                    BringGameToFront();
                    if (_gameReady) { shown = true; break; }
                    await Task.Delay(100);
                }
                if (!shown)
                {
                    try { _gameProcess.Kill(); } catch { }
                    _gameProcess = null;
                    await LaunchGameNow();
                    shown = _gameProcess != null;
                }
            }
            else
            {
                await LaunchGameNow();
                shown = _gameProcess != null;
            }

            if (_gameProcess != null && _gameProcess.HasExited)
            {
                string log = Path.Combine(_buildDir, "play.log");
                string why = File.Exists(log) ? Tail(File.ReadAllText(log)) : "The game exited before its window opened.";
                MessageBox.Show("The game stopped as it was starting.\n\n" + why, "OpenSmash");
                return;
            }
            if (!shown)
            {
                MessageBox.Show("Could not start the game. build\\ssbu.exe was not found.", "OpenSmash");
                return;
            }
            _waitingForGame = true;
            if (_launchMode != "vanilla" && _gameProcess != null)
            {
                var proc = _gameProcess;
                Hide();
                ShowInTaskbar = false;
                await Task.Run(() => { try { proc.WaitForExit(); } catch { } });
            }
            Close();
        }

        void LoadLaunchMode()
        {
            _launchMode = "mods";
            try
            {
                string path = Path.Combine(_buildDir, "launch_mode.txt");
                if (File.Exists(path) && File.ReadAllText(path).Trim().Equals("vanilla", StringComparison.OrdinalIgnoreCase))
                    _launchMode = "vanilla";
            }
            catch { }
        }

        void CycleLaunchMode(int delta)
        {
            string[] modes = { "mods", "vanilla" };
            int i = Array.IndexOf(modes, _launchMode);
            if (i < 0) i = 0;
            i = (i + delta) % modes.Length;
            if (i < 0) i += modes.Length;
            _launchMode = modes[i];
            try
            {
                Directory.CreateDirectory(_buildDir);
                File.WriteAllText(Path.Combine(_buildDir, "launch_mode.txt"), _launchMode + "\n");
            }
            catch { }
            Invalidate();
        }

        static string Tail(string text)
        {
            if (string.IsNullOrEmpty(text)) return "";
            return text.Length <= 1200 ? text : text.Substring(text.Length - 1200);
        }

        int RunPython(string[] args, out string output) => RunPython(args, out output, null);

        int RunPython(string[] args, out string output, Action<string> onLine)
        {
            var sb = new StringBuilder();
            var psi = new ProcessStartInfo
            {
                FileName = _pythonExe,
                WorkingDirectory = _repo,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            foreach (string a in args) psi.ArgumentList.Add(a);
            using (var p = Process.Start(psi))
            {
                if (p == null) { output = "python did not start"; return 1; }
                void Take(string line)
                {
                    if (line == null) return;
                    lock (sb) sb.AppendLine(line);
                    if (onLine != null) { try { onLine(line); } catch { } }
                }
                p.OutputDataReceived += (_, e) => Take(e.Data);
                p.ErrorDataReceived += (_, e) => Take(e.Data);
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                lock (sb) output = sb.ToString();
                return p.ExitCode;
            }
        }

        async Task LaunchGameNow()
        {
            try
            {
                if (!WindowSmash.IsBuilt(_repo))
                {
                    MessageBox.Show("The game isn't built here yet.", "OpenSmash");
                    return;
                }
                SetLoad("STARTING THE GAME", -1);
                // Same start as Play SSBU.bat: build\ssbu.exe dump\modules --status 30 --online
                _gameProcess = WindowSmash.StartGame(_repo);
                _gameLaunched = true;
                if (_gameProcess != null)
                {
                    SetLoad("STARTING THE GAME", 0.05f);
                    for (int i = 0; i < 450 && !_gameProcess.HasExited; i++)
                    {
                        if (FindVisibleGameWindow() != IntPtr.Zero)
                        {
                            SetLoad("STARTING THE GAME", 1);
                            _gameReady = true;
                            break;
                        }
                        SetLoad("STARTING THE GAME", Math.Min(0.92f, 0.08f + i / 450f));
                        await Task.Delay(100);
                    }
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Could not start the game.\n\n" + ex.Message, "OpenSmash-Ultimate");
            }
            finally
            {
                ClearLoad();
            }
        }

        static void DropEnvPrefix(ProcessStartInfo psi, string prefix)
        {
            var drop = new List<string>();
            foreach (System.Collections.DictionaryEntry e in psi.EnvironmentVariables)
            {
                string key = (string)e.Key;
                if (key.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) drop.Add(key);
            }
            foreach (string key in drop) psi.EnvironmentVariables.Remove(key);
        }

        void BringGameToFront()
        {
            if (_gameProcess == null || _gameProcess.HasExited) return;

            try
            {
                IntPtr hwnd = FindGameWindow();
                if (hwnd != IntPtr.Zero)
                {
                    // Restore and raise once — never leave HWND_TOPMOST pinned.
                    ShowWindow(hwnd, 9); // SW_RESTORE
                    SetWindowPos(hwnd, new IntPtr(0), 0, 0, 0, 0, 0x0001 | 0x0002); // HWND_TOP, SWP_NOMOVE | SWP_NOSIZE
                    SetForegroundWindow(hwnd);
                    _gameReady = true;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Bring to front failed: {ex.Message}");
            }
        }

        IntPtr FindGameWindow() => FindGameWindow(false);

        IntPtr FindVisibleGameWindow() => FindGameWindow(true);

        IntPtr FindGameWindow(bool visibleOnly)
        {
            if (_gameProcess == null) return IntPtr.Zero;
            var finder = new WindowFinder { TargetProcessId = _gameProcess.Id, VisibleOnly = visibleOnly };
            EnumWindows(finder.Callback, IntPtr.Zero);
            return finder.FoundHandle;
        }

        // Helper class for EnumWindows callback
        class WindowFinder
        {
            public int TargetProcessId;
            public bool VisibleOnly;
            public IntPtr FoundHandle = IntPtr.Zero;

            public bool Callback(IntPtr hWnd, IntPtr lParam)
            {
                GetWindowThreadProcessId(hWnd, out uint pid);
                if (pid == (uint)TargetProcessId && (!VisibleOnly || IsWindowVisible(hWnd)))
                {
                    FoundHandle = hWnd;
                    return false;
                }
                return true;
            }
        }

        // P/Invoke for window management
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool IsWindowVisible(IntPtr hWnd);
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool SetForegroundWindow(IntPtr hWnd);
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
        // ---------------------------------------------------------------------------
        // Fade-in animation
        async Task FadeIn()
        {
            Opacity = 0;
            for (int i = 1; i <= 12; i++)
            {
                Opacity = i / 12.0;
                await Task.Delay(12);
            }
            Opacity = 1;
        }

        // ---------------------------------------------------------------------------
        // WebView2 Marketplace (MOSAIC-style)
        // ---------------------------------------------------------------------------
        // WebView2 marketplace: host side of the JS bridge
        // ---------------------------------------------------------------------------
        readonly HashSet<string> _marketplaceInstalling = new();

        // Downloads get their own client with a long timeout (ProjectSetup's, which the first start uses too).
        static HttpClient _downloadClient => ProjectSetup.DownloadClient;

        async Task InitializeMarketplaceWebView()
        {
            if (_marketplaceInitialized || _marketplaceWebView != null) return;

            var view = new WebView2
            {
                Dock = DockStyle.None,   // the form positions it manually in PaintModsBrowser
                BackColor = C_BG,
                DefaultBackgroundColor = Color.Transparent,
                Visible = false,
            };
            Controls.Add(view);

            try
            {
                // A stable profile keeps the HTTP cache (thumbnails, CDN assets) across
                // launches; the old throwaway temp profile re-downloaded everything.
                string userDataFolder = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "OpenSmash", "WebView2");
                Directory.CreateDirectory(userDataFolder);

                var env = await CoreWebView2Environment.CreateAsync(null, userDataFolder);
                await view.EnsureCoreWebView2Async(env);

                var core = view.CoreWebView2;
                core.Settings.AreDefaultContextMenusEnabled = false;
#if DEBUG
                core.Settings.AreDevToolsEnabled = true;
#else
                core.Settings.AreDevToolsEnabled = false;
#endif
                core.Settings.IsStatusBarEnabled = false;
                core.Settings.AreHostObjectsAllowed = false;
                core.Settings.IsZoomControlEnabled = false;

                // The pane only ever shows the injected marketplace page; anything that
                // tries to navigate away opens in the user's real browser instead.
                core.NavigationStarting += (_, args) =>
                {
                    if (args.Uri.StartsWith("about:", StringComparison.OrdinalIgnoreCase)) return;
                    if (args.Uri.StartsWith("data:", StringComparison.OrdinalIgnoreCase)) return;
                    args.Cancel = true;
                    OpenExternal(args.Uri);
                };

                core.WebMessageReceived += OnMarketplaceMessage;

                _marketplaceWebView = view;
                _marketplaceInitialized = true;
                _marketplaceError = "";

                core.NavigateToString(GetMarketplaceHtml());
            }
            catch (Exception ex)
            {
                _marketplaceError = "Mod browser unavailable: " + ex.Message +
                                    "\nInstall the Microsoft Edge WebView2 Runtime, then restart OpenSmash.";
                try { Controls.Remove(view); view.Dispose(); } catch { }
                _marketplaceWebView = null;
            }

            Invalidate();
        }

        void OnMarketplaceMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
        {
            try
            {
                using var doc = JsonDocument.Parse(e.WebMessageAsJson);
                var root = doc.RootElement;

                string type = GetString(root, "type");
                string modId = GetString(root, "modId");
                string name = GetString(root, "name");
                string url = GetString(root, "url");

                switch (type)
                {
                    case "ready":
                        _ = PushInstalledState();
                        break;
                    case "install":
                        if (!string.IsNullOrEmpty(modId))
                        {
                            // The detail view resolves the exact file the user clicked, so
                            // pass it through. Anything that isn't a GameBanana URL is
                            // ignored and the file is looked up server-side instead.
                            _ = InstallMarketplaceMod(
                                modId,
                                GetString(root, "model"),
                                name,
                                GetString(root, "url"),
                                GetString(root, "fileName"),
                                GetString(root, "fileId"));
                        }
                        break;
                    case "remove":
                        if (!string.IsNullOrEmpty(modId)) _ = RemoveMarketplaceMod(modId, name);
                        break;
                    case "install_component":
                        // Requirements that are not mods (Skyline, ARCropolis) are
                        // installed by the host from their own release feeds.
                        _ = InstallComponentAsync(GetString(root, "component"));
                        break;
                    case "install_plugin":
                        // Skyline plugins are GitHub releases containing .nro files.
                        _ = InstallPluginAsync(GetString(root, "name"), GetString(root, "url"));
                        break;
                    case "open_external":
                        OpenExternal(url);
                        break;
                    case "detail":
                        // The page shows a mod's details; it has its own back link to
                        // the grid, so the host's back link is hidden while it is open.
                        bool detailOpen = root.TryGetProperty("open", out var openElement) &&
                                          openElement.ValueKind == JsonValueKind.True;
                        if (_marketplaceDetail != detailOpen)
                        {
                            _marketplaceDetail = detailOpen;
                            Invalidate();
                        }
                        break;
                    case "escape":
                        // The page owns the keyboard while focused, so it asks the host
                        // to return to the previous screen when the user presses Esc.
                        GoBack();
                        break;
                }
            }
            catch
            {
                // A malformed message from the page must never take the launcher down.
            }
        }

        // Hands a URL to the user's browser, but only a plain http(s) one: the page and
        // the mods it shows are not trusted to start programs or open local files.
        static bool OpenExternal(string? url)
        {
            if (string.IsNullOrWhiteSpace(url)) return false;
            if (!Uri.TryCreate(url, UriKind.Absolute, out var uri)) return false;
            if (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp) return false;
            try
            {
                Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true });
                return true;
            }
            catch { return false; }
        }

        static string GetString(JsonElement root, string property)
        {
            if (!root.TryGetProperty(property, out var value)) return "";
            return value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : value.ToString();
        }

        // ---------------------------------------------------------------------------
        // Install / remove
        // ---------------------------------------------------------------------------
        async Task InstallMarketplaceMod(string modId, string model, string name, string url, string fileName, string fileId)
        {
            if (string.IsNullOrWhiteSpace(name)) name = "mod " + modId;

            if (!_marketplaceInstalling.Add(modId))
            {
                await SendStatus(modId, "downloading", "Already installing " + name + "…", 0);
                return;
            }

            try
            {
                await SendStatus(modId, "downloading", "Installing " + name, 0);
                string why = "";
                await Task.Run(() =>
                {
                    try
                    {
                        var args = string.IsNullOrEmpty(fileId) ? new[] { "install", modId } : new[] { "install", modId, fileId };
                        OsMods(args, line =>
                        {
                            if (!line.StartsWith("LOG ")) return;
                            string msg = line.Substring(4).Trim();
                            var pct = Regex.Match(msg, @"^percent (\d+)$");
                            if (pct.Success && int.TryParse(pct.Groups[1].Value, out int n))
                                _ = SendStatus(modId, n >= 100 ? "installing" : "downloading", "Installing " + name, Math.Clamp(n, 0, 100));
                            else if (msg.Length > 0 && msg.Length < 180)
                                _ = SendStatus(modId, "installing", msg, 92);
                        });
                    }
                    catch (Exception ex) { why = ex.Message; }
                });
                if (why.Length > 0)
                {
                    await SendStatus(modId, "error", why, 0);
                    return;
                }
                await PushInstalledState();
                await SendStatus(modId, "done", name + " installed and turned on.", 100);
            }
            catch (Exception ex)
            {
                await SendStatus(modId, "error", "Install failed: " + ex.Message, 0);
            }
            finally
            {
                _marketplaceInstalling.Remove(modId);
            }
        }

        // Skyline / ARCropolis cannot be installed as normal mod folders, so the
        // marketplace asks for them by component name instead.
        async Task InstallComponentAsync(string component)
        {
            component = (component ?? "").Trim().ToLowerInvariant();
            if (component != "skyline" && component != "arcropolis") return;

            string id = "component:" + component;
            if (!_marketplaceInstalling.Add(id)) return;

            string name = component == "arcropolis" ? "ARCropolis" : "Skyline";
            var pin = Pinned(component);
            string target = component == "arcropolis"
                ? Path.Combine(_repo, "sd", "ultimate", "arcropolis")
                : _skylineDir;

            try
            {
                await SendStatus(id, "downloading", $"Downloading {name} {pin.Version}…", 10);
                Directory.CreateDirectory(target);
                await DownloadPinnedAndExtract(pin, target);
                ImportPlugins(target);

                await SendStatus(id, "done", name + " installed.", 100);
                await PushInstalledState();
                Invalidate();
            }
            catch (Exception ex)
            {
                await SendStatus(id, "error", name + " install failed: " + ex.Message, 0);
            }
            finally
            {
                _marketplaceInstalling.Remove(id);
            }
        }

        // Skyline plugins (NRO Hook, Smashline, paramconfig...) ship as GitHub
        // releases with .nro files; they live in the layered-fs plugin folder.
        async Task InstallPluginAsync(string name, string url)
        {
            name = string.IsNullOrWhiteSpace(name) ? "Plugin" : name.Trim();
            string id = "plugin:" + name;
            if (!_marketplaceInstalling.Add(id)) return;

            string temp = Path.Combine(Path.GetTempPath(), "OpenSmash_plugin_" + Guid.NewGuid().ToString("N")[..8]);

            try
            {
                await SendStatus(id, "downloading", "Fetching " + name + "…", 0);

                // Only the plugins pinned in PinnedDownloads are downloaded; the page's
                // URL only tells which one it means. Anything else opens in the user's
                // browser for them to judge.
                var match = Regex.Match(url ?? "", @"^https?://(?:www\.)?github\.com/([^/?#]+)/([^/?#]+)", RegexOptions.IgnoreCase);
                string repo = match.Success ? match.Groups[1].Value + "/" + match.Groups[2].Value.TrimEnd('/') : "";
                if (repo.EndsWith(".git", StringComparison.OrdinalIgnoreCase)) repo = repo[..^4];

                if (RuntimeProvidedPluginRepos.Any(r => r.Equals(repo, StringComparison.OrdinalIgnoreCase)))
                {
                    await SendStatus(id, "done", name + " is built into OpenSmash; nothing to install.", 100);
                    return;
                }

                var pin = repo.Length > 0 ? PinnedForRepo(repo) : null;
                if (pin == null || pin.Key == "skyline" || pin.Key == "arcropolis")
                {
                    OpenExternal(url);
                    await SendStatus(id, "error", name + " is not one of the plugins OpenSmash installs; opened its page.", 0);
                    return;
                }

                await SendStatus(id, "downloading", $"Downloading {pin.Name} {pin.Version}…", 15);
                Directory.CreateDirectory(temp);
                await DownloadPinnedAndExtract(pin, temp);

                ImportPlugins(temp);
                if (!File.Exists(Path.Combine(_pluginsDir, Path.GetFileName(new Uri(pin.Url).AbsolutePath))))
                {
                    await SendStatus(id, "error", name + " has no plugin files.", 0);
                    return;
                }

                await SendStatus(id, "done", name + " installed.", 100);
                await PushInstalledState();
                Invalidate();
            }
            catch (Exception ex)
            {
                await SendStatus(id, "error", name + " install failed: " + ex.Message, 0);
            }
            finally
            {
                _marketplaceInstalling.Remove(id);
                try { if (Directory.Exists(temp)) Directory.Delete(temp, true); } catch { }
            }
        }

        async Task RemoveMarketplaceMod(string modId, string name)
        {
            try
            {
                await Task.Run(() => OsMods(new[] { "remove-id", modId }));
                UpdateMods();
                await PushInstalledState();
                await SendStatus(modId, "done", (string.IsNullOrEmpty(name) ? "Mod" : name) + " removed.", 0);
            }
            catch (Exception ex)
            {
                await SendStatus(modId, "error", "Remove failed: " + ex.Message, 0);
            }
        }

        // ---------------------------------------------------------------------------
        // GameBanana file resolution + download
        // ---------------------------------------------------------------------------
        static bool IsGameBananaUrl(string url)
        {
            if (!Uri.TryCreate(url, UriKind.Absolute, out var uri)) return false;
            if (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp) return false;

            string host = uri.Host;
            return host.Equals("gamebanana.com", StringComparison.OrdinalIgnoreCase) ||
                   host.EndsWith(".gamebanana.com", StringComparison.OrdinalIgnoreCase);
        }

        static string NormalizeModel(string model)
        {
            if (string.IsNullOrWhiteSpace(model)) return "Mod";
            foreach (char c in model)
                if (!char.IsLetter(c)) return "Mod";
            return model;
        }

        async Task<(string Url, string FileName)?> ResolveModFile(string modId, string model)
        {
            string route = NormalizeModel(model);
            string json = await _httpClient.GetStringAsync($"https://gamebanana.com/apiv12/{route}/{modId}/ProfilePage");

            using var doc = JsonDocument.Parse(json);
            if (!doc.RootElement.TryGetProperty("_aFiles", out var files) || files.ValueKind != JsonValueKind.Array)
                return null;

            bool found = false;
            (string Url, string FileName, long Added) best = ("", "", long.MinValue);

            foreach (var file in files.EnumerateArray())
            {
                // Archived files are superseded by a newer upload.
                if (file.TryGetProperty("_bIsArchived", out var archived) && archived.ValueKind == JsonValueKind.True)
                    continue;

                string url = GetString(file, "_sDownloadUrl");
                if (string.IsNullOrEmpty(url)) continue;

                long added = 0;
                if (file.TryGetProperty("_tsDateAdded", out var ts) && ts.TryGetInt64(out long parsed)) added = parsed;

                if (!found || added >= best.Added)
                {
                    best = (url, GetString(file, "_sFile"), added);
                    found = true;
                }
            }

            return found ? (best.Url, best.FileName) : null;
        }

        async Task DownloadWithProgressAsync(string url, string destination, Func<int, Task> onProgress)
        {
            await ProjectSetup.DownloadAsync(url, destination, async (received, total) =>
            {
                if (total > 0) await onProgress((int)(received * 100 / total));
            });
            await onProgress(100);
        }

        // ---------------------------------------------------------------------------
        // Archive extraction
        // ---------------------------------------------------------------------------
        async Task ExtractArchiveAsync(string archive, string destination)
        {
            string extension = Path.GetExtension(archive).ToLowerInvariant();

            if (extension == ".zip")
            {
                System.IO.Compression.ZipFile.ExtractToDirectory(archive, destination, true);
                return;
            }

            // GameBanana mods are frequently .7z or .rar, which ZipFile cannot read.
            string? sevenZip = FindSevenZip();
            if (sevenZip != null)
            {
                await RunToolAsync(sevenZip, $"x -y -o\"{destination}\" \"{archive}\"");
                return;
            }

            // bsdtar ships with Windows 10 1803+ and reads 7z/rar/tar natively.
            await RunToolAsync("tar.exe", $"-xf \"{archive}\" -C \"{destination}\"");
        }

        static string? FindSevenZip()
        {
            var candidates = new List<string>
            {
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "7-Zip", "7z.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "7-Zip", "7z.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "7-Zip", "7z.exe"),
            };

            foreach (string candidate in candidates)
                if (File.Exists(candidate)) return candidate;

            // Fall back to PATH lookup via the Windows shell.
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = "where.exe",
                    Arguments = "7z.exe",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                };
                using var process = Process.Start(psi);
                if (process != null)
                {
                    string output = process.StandardOutput.ReadToEnd().Trim();
                    process.WaitForExit(3000);
                    string first = output.Split('\n').FirstOrDefault()?.Trim() ?? "";
                    if (first.Length > 0 && File.Exists(first)) return first;
                }
            }
            catch { }

            return null;
        }

        static async Task RunToolAsync(string fileName, string arguments)
        {
            var psi = new ProcessStartInfo
            {
                FileName = fileName,
                Arguments = arguments,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };

            using var process = Process.Start(psi)
                ?? throw new InvalidOperationException($"Could not start {fileName}.");

            string stderr = await process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();

            if (process.ExitCode != 0)
                throw new InvalidOperationException(
                    $"{Path.GetFileName(fileName)} failed (exit {process.ExitCode}): {stderr.Trim()}");
        }

        // Smart installer: scan an extracted archive and route files based on their
        // structure instead of blindly copying everything into one mod folder.
        // Returns the primary mod folder that received files (for marker writing),
        // or null when only plugins/configs were installed.
        string SmartInstallExtracted(string extracted, string modId, string name, string archiveName)
        {
            Directory.CreateDirectory(_modsDir);
            Directory.CreateDirectory(_pluginsDir);
            Directory.CreateDirectory(_skylineDir);

            var allFiles = Directory.GetFiles(extracted, "*", SearchOption.AllDirectories);
            bool hasModsTree = allFiles.Any(f => ContainsSegment(f, extracted, "ultimate", "mods"));
            bool hasPlugins = allFiles.Any(f => f.EndsWith(".nro", StringComparison.OrdinalIgnoreCase));
            bool hasAtmospherePlugins = allFiles.Any(f => ContainsSegment(f, extracted, "skyline", "plugins"));

            string primaryTarget = null;

            // 1. Structured mods: ultimate/mods/<modname>/... -> sd:/ultimate/mods/<modname>/
            if (hasModsTree)
            {
                foreach (string file in allFiles)
                {
                    string rel = Path.GetRelativePath(extracted, file);
                    string[] parts = rel.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                    // find "ultimate/mods" segment pair
                    int idx = -1;
                    for (int i = 0; i + 1 < parts.Length; i++)
                    {
                        if (parts[i].Equals("ultimate", StringComparison.OrdinalIgnoreCase) &&
                            parts[i + 1].Equals("mods", StringComparison.OrdinalIgnoreCase))
                        { idx = i + 2; break; }
                    }
                    if (idx < 0 || idx >= parts.Length) continue;
                    // tail = modname\...
                    string tail = string.Join(Path.DirectorySeparatorChar.ToString(), parts, idx, parts.Length - idx);
                    if (string.IsNullOrWhiteSpace(tail)) continue;
                    string dest = Path.Combine(_modsDir, tail);
                    Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                    File.Copy(file, dest, true);
                    string modFolder = Path.Combine(_modsDir, parts[idx]);
                    if (primaryTarget == null) primaryTarget = modFolder;
                }
            }

            // 2. Plugins: *.nro -> plugin dir. Prefer atmosphere/.../plugins/*.nro name.
            if (hasPlugins)
            {
                foreach (string file in allFiles.Where(f => f.EndsWith(".nro", StringComparison.OrdinalIgnoreCase)))
                {
                    string dest = Path.Combine(_pluginsDir, Path.GetFileName(file));
                    File.Copy(file, dest, true);
                }
            }

            // 3. Config files -> skyline dir (and atmosphere plugin dir if present)
            foreach (string file in allFiles)
            {
                string fn = Path.GetFileName(file);
                if (fn.Equals("config.ini", StringComparison.OrdinalIgnoreCase) ||
                    fn.Equals("config.json", StringComparison.OrdinalIgnoreCase))
                {
                    string dest = Path.Combine(_skylineDir, fn);
                    Directory.CreateDirectory(_skylineDir);
                    File.Copy(file, dest, true);
                }
            }

            // 4. Fallback: simple mod (no mods tree, no plugins) -> copy whole content root
            //    into a single mod folder, preserving previous behaviour.
            if (!hasModsTree && !hasPlugins)
            {
                string contentRoot = FindContentRoot(extracted);
                bool unwrapped = !string.Equals(contentRoot, extracted, StringComparison.OrdinalIgnoreCase);
                string folderName = unwrapped
                    ? SafeFolderName(Path.GetFileName(contentRoot), modId)
                    : SafeFolderName(name, modId);
                string target = Path.Combine(_modsDir, folderName);
                Directory.CreateDirectory(_modsDir);
                if (Directory.Exists(target)) Directory.Delete(target, true);
                Directory.CreateDirectory(target);
                CopyDirectory(contentRoot, target);
                primaryTarget = target;
            }

            return primaryTarget;
        }

        static bool ContainsSegment(string file, string extracted, params string[] segments)
        {
            string rel = Path.GetRelativePath(extracted, file);
            string[] parts = rel.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            for (int i = 0; i + segments.Length <= parts.Length; i++)
            {
                bool ok = true;
                for (int j = 0; j < segments.Length; j++)
                {
                    if (!parts[i + j].Equals(segments[j], StringComparison.OrdinalIgnoreCase)) { ok = false; break; }
                }
                if (ok) return true;
            }
            return false;
        }

        static string FindContentRoot(string extracted)
        {
            try
            {
                var entries = Directory.GetFileSystemEntries(extracted);
                if (entries.Length == 1 && Directory.Exists(entries[0])) return entries[0];
            }
            catch { }
            return extracted;
        }

        static string SafeFileName(string suggested, string url)
        {
            string name = string.IsNullOrWhiteSpace(suggested)
                ? Path.GetFileName(new Uri(url).AbsolutePath)
                : suggested;

            name = Path.GetFileName(name);
            foreach (char invalid in Path.GetInvalidFileNameChars())
                name = name.Replace(invalid, '_');

            return string.IsNullOrWhiteSpace(name) ? "mod_download.zip" : name;
        }

        static string SafeFolderName(string name, string modId)
        {
            var builder = new StringBuilder();
            foreach (char c in name.Trim())
            {
                if (Path.GetInvalidFileNameChars().Contains(c)) builder.Append('_');
                else builder.Append(c);
            }

            string cleaned = builder.ToString().Trim().TrimEnd('.');
            if (cleaned.Length > 64) cleaned = cleaned[..64].Trim();
            return cleaned.Length == 0 ? "mod_" + modId : cleaned;
        }

        // ---------------------------------------------------------------------------
        // Installed-state sync (persisted as a marker file inside each mod folder)
        // ---------------------------------------------------------------------------
        const string ModMarkerName = ".opensmash.json";

        string? FindInstalledModFolder(string modId)
        {
            if (!Directory.Exists(_modsDir)) return null;

            foreach (string directory in Directory.GetDirectories(_modsDir))
            {
                string marker = Path.Combine(directory, ModMarkerName);
                if (!File.Exists(marker)) continue;

                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(marker));
                    if (GetString(doc.RootElement, "gamebananaId") == modId) return directory;
                }
                catch { }
            }

            // Folders created by the older installer build.
            string legacy = Path.Combine(_modsDir, "mod_" + modId);
            return Directory.Exists(legacy) ? legacy : null;
        }

        List<string> GetInstalledModIds()
        {
            var ids = new List<string>();
            foreach (var m in _mods)
                if (!string.IsNullOrEmpty(m.GamebananaId)) ids.Add(m.GamebananaId);
            return ids;
        }

        async Task PushInstalledState()
        {
            // Read status before publishing installed IDs, including after a local import.
            await RefreshMods();
            var core = _marketplaceWebView?.CoreWebView2;
            if (core == null) return;

            string installed = JsonSerializer.Serialize(GetInstalledModIds());
            string inventory = JsonSerializer.Serialize(new
            {
                moddingReady = ModdingInstalled,
                plugins = PluginFileNames().Append("libparam_config").Distinct().ToArray(),
                mods = Directory.Exists(_modsDir)
                    ? Directory.GetDirectories(_modsDir).Select(Path.GetFileName).ToArray()
                    : Array.Empty<string>(),
                loaders = new
                {
                    skyline = _built || File.Exists(Path.Combine(_repo, "build", "ssbu.exe")),
                    arcropolis = File.Exists(Path.Combine(_pluginsDir, "libarcropolis.nro"))
                }
            });

            try
            {
                await core.ExecuteScriptAsync(
                    $"window.__osm && (window.__osm.setInstalled({installed}), window.__osm.setInventory({inventory}));");
            }
            catch { }
        }

        async Task SendStatus(string modId, string state, string message, double percent)
        {
            // Download progress arrives on the python reader's thread. WebView2 only
            // accepts script from the window thread, so those updates were dropped
            // and the bar stayed at 0.
            if (InvokeRequired)
            {
                var done = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                try
                {
                    BeginInvoke(new Action(() => _ = FinishStatus(modId, state, message, percent, done)));
                }
                catch (InvalidOperationException)
                {
                    return;
                }
                try { await done.Task; } catch { }
                return;
            }
            await FinishStatus(modId, state, message, percent, null);
        }

        async Task FinishStatus(string modId, string state, string message, double percent, TaskCompletionSource<bool> done)
        {
            try
            {
                var core = _marketplaceWebView?.CoreWebView2;
                if (core != null)
                {
                    string payload = JsonSerializer.Serialize(new { modId, state, message, percent });
                    await core.ExecuteScriptAsync($"window.__osm && window.__osm.status({payload});");
                }
                done?.TrySetResult(true);
            }
            catch
            {
                done?.TrySetResult(true);
            }
        }

        // ---------------------------------------------------------------------------
        // The marketplace UI ships as MarketplaceHtml.html (embedded resource) so the
        // markup and JavaScript stay plain UTF-8 instead of an escaped C# string.
        static string GetMarketplaceHtml()
        {
            var assembly = System.Reflection.Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("MarketplaceHtml.html");

            if (stream == null)
            {
                const string fallback = "<html><body style='font-family:Segoe UI;background:#08090b;color:#f2f3f5;padding:40px'>" +
                                        "<h2>Marketplace UI missing</h2>" +
                                        "<p>The embedded resource <code>MarketplaceHtml.html</code> was not found in this build.</p>" +
                                        "</body></html>";
                return fallback;
            }

            using var reader = new StreamReader(stream, Encoding.UTF8);
            return reader.ReadToEnd();
        }
    }

    // ---------------------------------------------------------------------------
    // Helpers
    // P/Invoke delegate for EnumWindows
        delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        class Hit { public string Id; public float X, Y, W, H; }

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
        public bool IsResolution = false; // when true, value is "WxH" from Values
    }

    class ResolutionSetting
    {
        public string Key = "resolution";
        public string Label = "Resolution";
        public string[] Values = new[]
        {
            "1920x1080", "2560x1440", "3840x2160",
            "1280x720", "1600x900", "2048x1152", "2880x1620"
        };
    }

    class EmulatorInfo
    {
        public string Name;
        public string Path;
        public string SdPath;
        public int ModCount = 0;
        public int PluginCount = 0;
    }
}


