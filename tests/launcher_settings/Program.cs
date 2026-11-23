// The launcher's settings against the game's: a fresh folder shows the game's defaults and writes nothing, what the
// game's settings menu saved is read back and never written over, the online lobby line round-trips as the game reads
// it; the remap page's controllers.ini sections (simulated pads: no controller is needed); the home page's lobby feed.
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Windows.Forms;
using OpenSmash.Launcher;

static class Tests
{
    static int count;
    static void Check(bool ok, string name) { if (!ok) throw new Exception("FAIL " + name); count++; Console.WriteLine("PASS " + name); }

    sealed class FakePads : IPadSource
    {
        public string Problem { get; set; } = "";
        public List<PadDevice> List = new();
        public HashSet<string> Held = new();
        public bool Disposed;
        public IReadOnlyList<PadDevice> Pads() => List.ToList();
        public ISet<string> Pressed(uint id) => new HashSet<string>(Held);
        public void Dispose() => Disposed = true;
    }

    sealed class Refuse : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            throw new HttpRequestException("no network in tests");
    }

    sealed class Serve : HttpMessageHandler
    {
        public Dictionary<string, string> Pages = new();
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(Pages.TryGetValue(request.RequestUri!.AbsolutePath, out var body)
                ? new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(body) }
                : new HttpResponseMessage(HttpStatusCode.NotFound));
    }

    [STAThread]
    static int Main()
    {
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        string root = Path.Combine(Path.GetTempPath(), "launcher settings tests " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            Lobby();
            OnlineRules(Path.Combine(root, "online"));
            FreshFolder(Path.Combine(root, "fresh"));
            GameChangesKept(Path.Combine(root, "game"));
            SameFilesAsTheGame(Path.Combine(root, "sync"));
            Profiles();
            Remap(Path.Combine(root, "remap"));
            Feed().GetAwaiter().GetResult();
            Presence(Path.Combine(root, "presence"));
            ShaderRebuild(Path.Combine(root, "shaders"));
            UpdateChannel().GetAwaiter().GetResult();
            // (a real SDL3.dll, when one is given: it loads and lists the pads, none needed)
            string? sdl = Environment.GetEnvironmentVariable("LAUNCHER_TEST_SDL_DIR");
            if (!string.IsNullOrEmpty(sdl))
            {
                using var real = new SdlPadSource(sdl, sdl);
                Check(real.Problem == "", "SDL3.dll loads: " + real.Problem);
                var list = real.Pads();
                Console.WriteLine($"SDL lists {list.Count} pad(s): " + string.Join("; ", list.Select(p => $"{p.Name} ({p.Type}, GUID {p.Guid})")));
                foreach (var p in list) Check(p.Guid.Length == 32, "GUID as SDL_GUIDToString writes it");
            }
            string? render = Environment.GetEnvironmentVariable("LAUNCHER_TEST_RENDER");
            if (!string.IsNullOrEmpty(render)) Render(Path.Combine(root, "render"), render);
            using (var missing = new SdlPadSource(Path.Combine(root, "nowhere"), root))
                Check(missing.Problem != "" && missing.Pads().Count == 0, "no SDL3.dll: a reason, no pads");
            Console.WriteLine($"{count} checks passed");
            return 0;
        }
        catch (Exception e) { Console.WriteLine(e); return 1; }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }

    // Settings > Storage > REBUILD SHADERS: which files of dump\shadercache go (ShaderCache.RebuildTargets), and that the
    // game's other files, a file the list does not know and drawn_keys.txt stay.
    static void ShaderRebuild(string repo)
    {
        Check(ShaderCache.RebuildTargets(repo).Count == 0, "no shader cache: nothing to delete");
        string dir = Path.Combine(repo, "dump", "shadercache");
        Directory.CreateDirectory(Path.Combine(dir, "nvidia", "GLCache", "38af177a636d64eb6428bd306291f7a8"));
        File.WriteAllText(Path.Combine(dir, "nvidia", "GLCache", "38af177a636d64eb6428bd306291f7a8", "x.bin"), "x");
        string[] gone =
        {
            "complete.txt", "prepare_tries.txt", "warm.done", "prewarm-all.done", "warm.log", "warm.lock",
            "v10_vs_0123456789abcdef_000003ff_0000000000000000.dxbc",
            "v9_PS_0123456789abcdef_0000003f_0000000000000000.dxbc",
            "g1002.01100600_ps_4e9c6d74942d892b_00000007_0000000000000000.spv",
            "g1001_cs_4e9c6d74942d892b_00000000_0000000000000000.spv",
            "m1003_vs_4e9c6d74942d892b_000000ff_0000000000000000.metal",
            "vulkan_10de_2d05.pipelines", "vulkan_10de_2d05.warm", "vulkan_10de_2d05.warm.warming", "vulkan_10de_2d05.warm.capped",
            "vulkan_10de_2d05.pipelines.1234.tmp", "vulkan_10de_2d05.pipelines.vk99.tmp",
        };
        string[] kept = { "drawn_keys.txt", "notes.txt", "vulkan_pipelines.warm", "v10_vs_readme.txt", "g1002.spv" };
        foreach (var f in gone.Concat(kept)) File.WriteAllText(Path.Combine(dir, f), "x");
        string[] outside =
        {
            Path.Combine(repo, "dump", "save", "system.bin"), Path.Combine(repo, "dump", "data", "warm.done"),
            Path.Combine(repo, "dump", "modules", "complete.txt"), Path.Combine(repo, "mods", "a", "v10_vs_0123456789abcdef_000003ff_0000000000000000.dxbc"),
        };
        foreach (var f in outside) { Directory.CreateDirectory(Path.GetDirectoryName(f)!); File.WriteAllText(f, "x"); }

        var targets = ShaderCache.RebuildTargets(repo);
        string full = Path.GetFullPath(dir) + Path.DirectorySeparatorChar;
        Check(targets.All(t => Path.GetFullPath(t).StartsWith(full, StringComparison.OrdinalIgnoreCase)), "rebuild: only inside dump/shadercache");
        Check(targets.Count > 0 && Path.GetFileName(targets[0]) == "complete.txt", "rebuild: complete.txt goes first");
        var names = targets.Select(Path.GetFileName).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var f in gone) Check(names.Contains(f), "rebuild deletes " + f);
        Check(names.Contains("nvidia"), "rebuild deletes the game's nvidia folder");
        foreach (var f in kept) Check(!names.Contains(f), "rebuild keeps " + f);
        Check(targets.Count == gone.Length + 1, "rebuild: nothing else");

        Check(ShaderCache.Clear(repo).Count == 0, "rebuild: everything deleted");
        foreach (var f in gone) Check(!File.Exists(Path.Combine(dir, f)), "deleted " + f);
        Check(!Directory.Exists(Path.Combine(dir, "nvidia")), "deleted nvidia folder");
        foreach (var f in kept) Check(File.Exists(Path.Combine(dir, f)), "kept " + f);
        foreach (var f in outside) Check(File.Exists(f), "kept outside the cache: " + f);
        Check(ShaderCache.RebuildTargets(repo).Count == 0, "rebuild: a second run finds nothing");
    }

    // online_server.txt's lines as the game's settings menu reads and writes them (runtime/gpu_menu.c net_load / net_save_server)
    static void Lobby()
    {
        string pub = SettingDefaults.PublicLobby;
        var cases = new (string Line, string Lobby, string Back)[]
        {
            ("", "offline", "off"), ("off", "offline", "off"), ("auto", "auto", "auto"), ("na", "auto", "auto"), ("eu", "auto", "auto"),
            ("smash.recomps.wtf:39421", "auto", "auto"), ("smash.recomps.wtf:39421 mine", "private", pub + " mine"),
            ("opensmash-lobby.duckdns.org:39421", "auto", "auto"), ("opensmash-lobby.duckdns.org:39421 mine", "private", pub + " mine"),
            ("eos", "auto", "auto"), ("EOS", "auto", "auto"), ("eos mine", "private", pub + " mine"),   // (Epic Online Services, gone in 0.5.0: the official lobby)
            ("lan", "lan", "lan"), (pub, "auto", "auto"), (pub + " friends", "private", pub + " friends"),
            ("10.0.0.2:39420", "custom", "10.0.0.2:39420"), ("10.0.0.2:39420 abc", "custom", "10.0.0.2:39420 abc"),
        };
        foreach (var c in cases)
        {
            var (lobby, code, server) = SettingDefaults.LobbyFromLine(c.Line);
            Check(lobby == c.Lobby, $"'{c.Line}' reads as {c.Lobby}");
            Check(SettingDefaults.LineFromLobby(lobby, code, server) == c.Back, $"'{c.Line}' written back as '{c.Back}'");
            Check(SettingDefaults.Choices["lobby"].Contains(lobby), $"{lobby} is a choice of the page");
        }
        Check(SettingDefaults.LineFromLobby("private", "", "") == pub + " friends", "private lobby without a code: the game's 'friends'");
        // (issue #389: an old launcher.ini's "Your own server" holding a public lobby's address is the Public lobby)
        Check(SettingDefaults.LineFromLobby("custom", "", "smash.recomps.wtf:39421") == "auto", "custom smash.recomps.wtf: the public lobby's auto");
        Check(SettingDefaults.LineFromLobby("custom", "abc", "smash.recomps.wtf:39421") == pub + " abc", "custom smash.recomps.wtf with a code: the private lobby");
        Check(SettingDefaults.LineFromLobby("custom", "", pub) == "auto", "custom with the public lobby's address: auto");
        Check(SettingDefaults.IsOldLobbyLine("smash.recomps.wtf:39421 x") && !SettingDefaults.IsOldLobbyLine("auto") && !SettingDefaults.IsOldLobbyLine(""), "old lobby lines");
        // the server region: any region's lobby in the file is "the official lobby" (the region setting says which one)
        foreach (var region in OpenSmash.Core.ServerRegion.All)
        {
            Check(SettingDefaults.LobbyFromLine(region.Lobby).Lobby == "auto" && SettingDefaults.LobbyFromLine(region.Lobby + " mine").Lobby == "private", $"{region.Lobby} reads as the official lobby");
            Check(SettingDefaults.Choices["region"].Contains(region.Id), $"{region.Id} is a choice of the page");
        }
        // (our regions only, by their names; "auto" stays the game's own default in the file)
        Check(SettingDefaults.Choices["region"].SequenceEqual(new[] { "eu", "na-east", "sa" }) && SettingDefaults.RegionLabels.SequenceEqual(new[] { "Europe", "North America East", "South America" })
              && SettingDefaults.OfFile("server_region.txt") == "auto", "Server region: Europe, North America East and South America");
        string install = Path.Combine(Path.GetTempPath(), "opensmash-region-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(Path.Combine(install, "config"));
        try
        {
            Check(SettingDefaults.RegionShown(install, "eu") == null && OpenSmash.Core.OnlineSettings.RegionShown(install) == "eu", "no region chosen, nothing measured: Europe");
            File.WriteAllText(Path.Combine(install, "config", "server_region_auto.txt"), "na-east 23 1700000000\n");
            Check(OpenSmash.Core.OnlineSettings.RegionShown(install) == "na-east", "auto: the region it plays on (its pick)");
            OpenSmash.Core.ServerRegion.SetSetting(install, "eu");
            Check(OpenSmash.Core.OnlineSettings.RegionShown(install) == "eu" && OpenSmash.Core.OfficialLobby.Current(install) == OpenSmash.Core.ServerRegion.ById("eu").Lobby, "a chosen region over the pick, and its lobby is the one played on");
            OpenSmash.Core.ServerRegion.SetSetting(install, "na-east");
            Check(OpenSmash.Core.OnlineSettings.RegionShown(install) == "na-east" && OpenSmash.Core.OfficialLobby.Current(install) == "na.lobby.opensmashultimate.com:39421", "North America East chosen: its lobby");
            File.WriteAllText(Path.Combine(install, "config", "official_lobby.txt"), "lobby.example:39421\n");
            Check(SettingDefaults.RegionShown(install, "auto") == "lobby.example:39421" && SettingDefaults.RegionShown(install, "eu") == "lobby.example:39421", "an install's own official server is shown instead");
        }
        finally { try { Directory.Delete(install, true); } catch (IOException) { } }
        Check(SettingDefaults.Renderer("d3d11") == "auto" && SettingDefaults.Renderer("DX11") == "direct3d11" && SettingDefaults.Renderer("vk") == "vulkan", "renderer names as runtime/renderer.h");
    }

    // The Online rows (OpenSmash.Core.OnlineSettings): the official lobby hides AND resets the lobby code, the server of
    // one's own and Direct connections (off), here and in launcher.ini's remembered copy; a server of one's own shows them
    // again, empty, Direct connections back on; nothing half-typed reaches online_server.txt; the server region is Europe
    // or North America East, saved, read back after a restart and the lobby played on. Old files' leftovers included.
    static void OnlineRules(string repo)
    {
        string config = Path.Combine(repo, "config");
        Directory.CreateDirectory(config);
        File.WriteAllText(Path.Combine(config, "version.ini"), "version = 1\n");
        string server = Path.Combine(config, "online_server.txt"), direct = Path.Combine(config, "online_direct.txt"), ini = Path.Combine(config, "launcher.ini");
        File.WriteAllText(server, "auto\n");
        File.WriteAllText(direct, "on\n");
        File.WriteAllText(ini, "[opensmash]\ncode = oldcode\nserver = my.host:39420\n");   // (an older launcher's leftovers)
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        string IniOf(string key) => File.ReadAllLines(ini).Select(l => l.Split('=')).Where(p => p.Length == 2 && p[0].Trim() == key).Select(p => p[1].Trim()).FirstOrDefault() ?? "";
        var (form, call, values) = Open(repo);
        using (form)
        {
            void Write(string key, string value) => typeof(LauncherForm).GetMethod("WriteSetting", flags)!.Invoke(form, new object[] { key, value });
            var settings = (Setting[])typeof(LauncherForm).GetField("_settings", flags)!.GetValue(form)!;
            bool On(string key) => (bool)typeof(LauncherForm).GetMethod("SettingOn", flags)!.Invoke(form, new object[] { settings.First(x => x.Key == key) })!;
            call("ReadSettings");
            Check(values["lobby"] == "auto" && values["code"] == "" && values["server"] == "" && values["direct"] == "off",
                  "official lobby: an old launcher.ini's code and server and an online_direct.txt 'on' are not shown");
            Check(!On("code") && !On("server") && !On("direct") && On("region") && On("name"), "official lobby: only the name and the server region are shown");
            Check(File.ReadAllText(server) == "auto\n" && File.ReadAllText(direct) == "on\n", "reading writes nothing");
            Write("lobby", "custom");
            Check(values["code"] == "" && values["server"] == "" && values["direct"] == "on" && On("code") && On("server") && On("direct") && !On("region"),
                  "your own server: code, server and direct connections shown again, empty, direct on");
            Check(File.ReadAllText(server) == "auto\n", "your own server without its address: online_server.txt keeps the last complete choice (no made-up 127.0.0.1)");
            Check(IniOf("code") == "" && IniOf("server") == "", "launcher.ini's stale code and server reset");
            Write("server", "my.host:39420");
            Check(File.ReadAllText(server).Trim() == "my.host:39420", "the server written once it is host:port");
            Write("code", "abc");
            Check(File.ReadAllText(server).Trim() == "my.host:39420 abc" && IniOf("code") == "abc", "with its code");
            Write("lobby", "auto");
            Check(values["code"] == "" && values["server"] == "" && values["direct"] == "off" && File.ReadAllText(server).Trim() == "auto"
                  && File.ReadAllText(direct).Trim() == "off" && IniOf("code") == "" && IniOf("server") == "",
                  "back to the official lobby: code and server reset everywhere, direct connections off");
            Write("region", "na-east");
            Check(OpenSmash.Core.ServerRegion.Setting(repo) == "na-east" && OpenSmash.Core.OfficialLobby.Current(repo) == "na.lobby.opensmashultimate.com:39421",
                  "North America East picked: saved, and its lobby is the one played on");
        }
        var (form2, call2, values2) = Open(repo);   // (a restart)
        using (form2)
        {
            void Write(string key, string value) => typeof(LauncherForm).GetMethod("WriteSetting", flags)!.Invoke(form2, new object[] { key, value });
            call2("ReadSettings");
            Check(values2["lobby"] == "auto" && values2["region"] == "na-east" && values2["code"] == "" && values2["server"] == "", "after a restart: the official lobby, North America East, nothing stale");
            Write("region", "eu");
            Check(File.ReadAllText(Path.Combine(config, "server_region.txt")).Trim() == "eu" && OpenSmash.Core.OfficialLobby.Current(repo) == OpenSmash.Core.ServerRegion.ById("eu").Lobby, "Europe picked: saved and played on");
            Write("lobby", "private");
            Check(values2["code"] == "" && values2["direct"] == "off" && File.ReadAllText(server).Trim() == "auto", "private lobby: an empty code, nothing written until it is typed");
            Write("code", "friends1");
            Check(File.ReadAllText(server).Trim() == SettingDefaults.PublicLobby + " friends1", "private lobby with its code");
            Write("lobby", "lan");
            Check(File.ReadAllText(server).Trim() == "lan" && values2["direct"] == "on" && File.ReadAllText(direct).Trim() == "on", "LAN: direct connections back on");
            Write("lobby", "auto");
            Write("lobby", "custom");
            Write("lobby", "auto");
            Check(File.ReadAllText(server).Trim() == "auto" && values2["code"] == "" && values2["server"] == "" && File.ReadAllText(direct).Trim() == "off", "back and forth: the official lobby, consistent");
        }
        File.WriteAllText(server, "");   // (an online_server.txt without a line: the official lobby)
        Check(OpenSmash.Core.OnlineSettings.EnsureLine(server) && File.ReadAllText(server).Trim() == "auto" && !OpenSmash.Core.OnlineSettings.EnsureLine(server), "an empty online_server.txt becomes the official lobby, once");
        File.WriteAllText(server, "# the player's\noff\n");
        Check(!OpenSmash.Core.OnlineSettings.EnsureLine(server) && File.ReadAllText(server) == "# the player's\noff\n", "an explicit choice (offline) is kept");
    }

    static (LauncherForm Form, Func<string, object?> Call, Dictionary<string, string> Values) Open(string repo)
    {
        var form = new LauncherForm(repo, repo, Path.Combine(repo, "build"), preview: true);
        var type = typeof(LauncherForm);
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        object? Call(string name) => type.GetMethod(name, flags, Type.EmptyTypes)!.Invoke(form, null);
        var values = (Dictionary<string, string>)type.GetField("_iniValues", flags)!.GetValue(form)!;
        return (form, Call, values);
    }

    static string[] Files(string repo) => Directory.GetFiles(repo, "*", SearchOption.AllDirectories)
        .Select(f => Path.GetRelativePath(repo, f)).Where(f => !f.StartsWith("build")).OrderBy(f => f).ToArray();

    // A fresh download: no config\ yet. The page shows the game's defaults and closing it writes nothing.
    static void FreshFolder(string repo)
    {
        Directory.CreateDirectory(repo);
        var (form, call, values) = Open(repo);
        using (form)
        {
            call("ReadSettings");
            foreach (var row in SettingDefaults.Table.Where(r => r.Key != ""))
                Check(values[row.Key] == row.Default || (row.Key == "renderer" && values[row.Key] == SettingDefaults.Renderer(row.Default)),
                      $"fresh: {row.Key} shows the game's default {row.Default} (shows {values.GetValueOrDefault(row.Key)})");
            // (the official lobby is the launchers' default: an unset online_server.txt is written "auto", nothing else)
            Check(values["lobby"] == "auto" && values["direct"] == "off" && values["code"] == "" && values["server"] == "" && values["name"] == "", "fresh: no online file is the official lobby, direct off");
            call("PersistSettings");
            Check(Files(repo).SequenceEqual(new[] { "online_server.txt" }) && File.ReadAllText(Path.Combine(repo, "online_server.txt")).Trim() == "auto",
                  "fresh: only the official lobby's line is written (" + string.Join(", ", Files(repo)) + ")");
            File.Delete(Path.Combine(repo, "online_server.txt"));
        }
        File.WriteAllText(Path.Combine(repo, "online_server.txt"), "# comment\nauto\n");   // (what the download comes with)
        var (form2, call2, values2) = Open(repo);
        using (form2)
        {
            call2("ReadSettings");
            Check(values2["lobby"] == "auto", "download's online_server.txt: the nearest public lobby");
            call2("PersistSettings");
            Check(File.ReadAllText(Path.Combine(repo, "online_server.txt")) == "# comment\nauto\n" && Files(repo).Length == 1, "download's files left as they are");
        }
        File.WriteAllText(Path.Combine(repo, "online_server.txt"), "# comment\nsmash.recomps.wtf:39421\n");   // (the gone NA lobby: issue #389)
        var (form3, call3, values3) = Open(repo);
        using (form3)
        {
            call3("ReadSettings");
            Check(values3["lobby"] == "auto", "the North American lobby's line reads as the public lobby");
            Check(File.ReadAllText(Path.Combine(repo, "online_server.txt")).Replace("\r\n", "\n") == "# comment\nauto\n", "the North American lobby's line written again as auto: "
                  + File.ReadAllText(Path.Combine(repo, "online_server.txt")));
        }
        File.WriteAllText(Path.Combine(repo, "online_server.txt"), "# comment\nauto\n");
    }

    // The game's settings menu (Esc) writes while the launcher waits hidden: closing the launcher keeps those changes,
    // and the next start shows them.
    static void GameChangesKept(string repo)
    {
        string config = Path.Combine(repo, "config");
        Directory.CreateDirectory(config);
        File.WriteAllText(Path.Combine(config, "version.ini"), "version = 1\n");
        File.WriteAllText(Path.Combine(config, "display.ini"), "window = windowed\nrenderer = auto\n");
        File.WriteAllText(Path.Combine(config, "online_server.txt"), "auto\n");
        var (form, call, values) = Open(repo);
        using (form)
        {
            call("ReadSettings");
            Check(values["renderer"] == "auto" && values["lobby"] == "auto", "read before the game");
            File.WriteAllText(Path.Combine(config, "display.ini"), "window = borderless\nrenderer = vulkan\n");   // (the game's menu)
            File.WriteAllText(Path.Combine(config, "online_server.txt"), "lan\n");
            File.WriteAllText(Path.Combine(config, "online_direct.txt"), "off\n");
            call("PersistSettings");   // (the launcher closes after the game)
            Check(File.ReadAllText(Path.Combine(config, "display.ini")) == "window = borderless\nrenderer = vulkan\n", "game's display.ini kept");
            Check(File.ReadAllText(Path.Combine(config, "online_server.txt")) == "lan\n", "game's lobby kept");
            Check(!File.Exists(Path.Combine(config, "launcher.ini")), "nothing remembered that was not changed here");
        }
        var (form2, call2, values2) = Open(repo);
        using (form2)
        {
            call2("ReadSettings");
            Check(values2["window"] == "borderless" && values2["renderer"] == "vulkan", "next start shows the game's display choices");
            Check(values2["lobby"] == "lan" && values2["direct"] == "off", "next start shows the game's lobby (LAN) and direct off");
            typeof(LauncherForm).GetMethod("WriteSetting", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form2, new object[] { "lobby", "auto" });
            Check(File.ReadAllText(Path.Combine(config, "online_server.txt")).Trim() == "auto", "picking the public lobby writes the game's 'auto'");
            Check(File.ReadAllText(Path.Combine(config, "display.ini")) == "window = borderless\nrenderer = vulkan\n", "a lobby change leaves display.ini alone");
            typeof(LauncherForm).GetMethod("WriteSetting", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form2, new object[] { "renderer", "auto" });
            Check(File.ReadAllText(Path.Combine(config, "display.ini")).Contains("renderer = auto"), "renderer back to auto");
        }
    }

    // One source of truth with the game's settings menu (Esc): the launcher writes a key as runtime/hle_settings.c does
    // (in place, comment and unknown keys kept, line ends kept, nothing written when unchanged), shows the renderer the
    // game will start with (#429's renderer_menu), and reads the files again when the game (or anyone) changed them.
    static void SameFilesAsTheGame(string repo)
    {
        string config = Path.Combine(repo, "config");
        Directory.CreateDirectory(config);
        File.WriteAllText(Path.Combine(config, "version.ini"), "version = 1\n");
        string display = Path.Combine(config, "display.ini"), pads = Path.Combine(config, "controllers.ini");
        File.WriteAllText(display, "; the game's\nwindow = windowed   ; mine\nrenderer = d3d11\nrenderer_menu = vulkan\nshow_advanced = 1\n");
        File.WriteAllText(pads, "[general]\r\nkeyboard_player = 1\r\n\r\n[players]\r\n; 2 = a pad\r\n");
        File.WriteAllText(Path.Combine(config, "online_server.txt"), "auto\n");
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var (form, call, values) = Open(repo);
        using (form)
        {
            void Write(string key, string value) => typeof(LauncherForm).GetMethod("WriteSetting", flags)!.Invoke(form, new object[] { key, value });
            call("ReadSettings");
            Check(values["renderer"] == "vulkan", "an older launcher's renderer = d3d11 under the menu's renderer_menu = vulkan shows Vulkan, as the game starts");
            Write("window", "borderless");
            Check(File.ReadAllText(display) == "; the game's\nwindow = borderless   ; mine\nrenderer = d3d11\nrenderer_menu = vulkan\nshow_advanced = 1\n",
                  "one key changed in place, its comment and the menu's own keys kept: " + File.ReadAllText(display));
            Write("renderer", "direct3d11");
            Check(File.ReadAllText(display).Contains("renderer = direct3d11\nrenderer_menu = direct3d11\n"), "the renderer written as the menu writes it (renderer_menu too)");
            Write("background", "0");
            Check(File.ReadAllText(pads) == "[general]\r\nkeyboard_player = 1\r\nbackground = 0\r\n\r\n[players]\r\n; 2 = a pad\r\n",
                  "a new key at the end of its section, CRLF kept: " + File.ReadAllText(pads).Replace("\r", "\r").Replace("\n", "\n"));
            var old = DateTime.UtcNow.AddMinutes(-10);
            File.SetLastWriteTimeUtc(display, old);
            Write("window", "borderless");
            Check(File.GetLastWriteTimeUtc(display) == old, "the same value again: the file is not written");

            // The files change while the launcher is open (the game's settings menu, a game started from a shortcut, the
            // "Open the file" row's editor): back in front, the page shows them; a lobby change not yet written stays.
            Write("lobby", "private");   // (no code yet: not written)
            File.WriteAllText(display, File.ReadAllText(display).Replace("window = borderless", "window = fullscreen").Replace("renderer = direct3d11", "renderer = vulkan"));
            File.WriteAllText(Path.Combine(config, "online_direct.txt"), "off\n");
            File.SetLastWriteTimeUtc(display, DateTime.UtcNow.AddSeconds(5));
            call("ReloadSettingsIfChanged");
            Check(values["window"] == "fullscreen" && values["renderer"] == "vulkan" && values["direct"] == "off", "the menu's changes shown after it");
            Check(values["lobby"] == "private", "a change made here and not written yet is kept");
            Check(File.ReadAllText(Path.Combine(config, "online_server.txt")) == "auto\n", "the unwritten lobby still not written");
            File.SetLastWriteTimeUtc(display, DateTime.UtcNow.AddSeconds(5));
            File.WriteAllText(Path.Combine(config, "online_name.txt"), "Rel\n");
            File.SetLastWriteTimeUtc(Path.Combine(config, "online_name.txt"), DateTime.UtcNow.AddSeconds(10));
            values["name"] = "stale";   // (a stale copy: only a reload puts it right)
            call("ReloadSettingsIfChanged");
            Check(values["name"] == "Rel", "read again on each change");
            call("PersistSettings");
            Check(File.ReadAllText(display).Contains("window = fullscreen") && File.ReadAllText(display).Contains("renderer = vulkan"), "closing keeps the menu's choices");
        }
    }

    static void Profiles()
    {
        string ini = "[general]\nkeyboard_player = 1\n[device default]\ndpad_moves = off\n" +
                     "[device XInput  Controller #1]   ; the menu's\nA = leftshoulder\n[device xboxone]\nB = north\nrumble = 50 # half\n";
        var all = PadProfile.Sections(ini);
        Check(all.ContainsKey("device XInput Controller #1"), "a section named with a '#' is whole");
        Check(all["device xboxone"]["rumble"] == "50", "'# half' is a comment");
        Check(PadProfile.Effective(ini, "guid", "XInput Controller #1", "xboxone", "A") == ("leftshoulder", "device XInput Controller #1"), "name's section for this pad");
        Check(PadProfile.Effective(ini, "guid", "XInput Controller #1", "xboxone", "B") == ("north", "device xboxone"), "type's section under it");
        Check(PadProfile.Effective(ini, "guid", "XInput Controller #2", "xboxone", "A").From == "", "another port's name: default A");
        string g = "0300fa67c82d00000631000014017801";
        string w = PadProfile.Set(ini, PadProfile.SectionFor(g), "A", "south", "XInput Controller #1 (xboxone)");
        Check(w.StartsWith(ini) && w.EndsWith("\n[device " + g + "]\n; XInput Controller #1 (xboxone)\nA = south\n"), "a new GUID section at the end, with the pad's name");
        Check(PadProfile.Effective(w, g, "XInput Controller #1", "xboxone", "A") == ("south", "device " + g), "GUID's section over the name's");
        w = PadProfile.Set(w, PadProfile.SectionFor(g), "dpad_moves", "on");
        w = PadProfile.Set(w, PadProfile.SectionFor(g), "A", "east");
        Check(w.Contains("[device " + g + "]\n; XInput Controller #1 (xboxone)\nA = east\ndpad_moves = on\n"), "keys replaced in place, added after the last");
        w = PadProfile.Set(w, PadProfile.SectionFor(g), "A", null);
        Check(!w.Contains("A = east") && PadProfile.Effective(w, g, "XInput Controller #1", "xboxone", "A").Value == "leftshoulder", "removed: the name's again");
        Check(PadProfile.Set("x = 1\r\n", "device a", "A", "south") == "x = 1\r\n\r\n[device a]\r\nA = south\r\n", "CRLF files stay CRLF");
        Check(PadProfile.NewlyPressed(new HashSet<string> { "south" }, new HashSet<string> { "south", "righttrigger" }) == "righttrigger", "a newly pressed control");
        Check(PadProfile.NewlyPressed(new HashSet<string> { "south" }, new HashSet<string> { "south" }) == null, "a held button does not count");
        Check(PadProfile.Buttons.All(PadProfile.Positional.ContainsKey) && PadProfile.Positional.Values.All(PadProfile.Sources.Contains), "every button has a default control");
    }

    static void Remap(string repo)
    {
        string config = Path.Combine(repo, "config");
        Directory.CreateDirectory(config);
        File.WriteAllText(Path.Combine(config, "version.ini"), "version = 1\n");
        File.WriteAllText(Path.Combine(config, "controllers.ini"), "; the game's file\n[general]\nkeyboard_player = 1\n");
        var pads = new FakePads();
        pads.List.Add(new PadDevice(7, "XInput Controller #1", "xboxone", "0300fa67c82d00000631000014017801"));
        pads.List.Add(new PadDevice(9, "DualSense Wireless Controller", "ps5", "050000004c050000e60c000000016800"));
        using var page = new RemapPage(repo, () => pads);
        page.Start();
        Check(page.Selected?.Id == 7, "the first pad is chosen");
        Check(page.Controls.Find("pad-1", true).Length == 1, "a button per pad");
        ((Button)page.Controls.Find("pad-1", true)[0]).PerformClick();
        Check(page.Selected?.Id == 9, "choosing the DualSense");
        ((Button)page.Controls.Find("pad-0", true)[0]).PerformClick();
        pads.Held.Add("south");   // (held when SET is pressed: not the answer)
        page.BeginCapture("A");
        Check(page.Capturing == "A", "waiting for a press");
        page.Poll();
        Check(page.Capturing == "A", "a button already held is not taken");
        pads.Held.Add("leftshoulder");
        page.Poll();
        string text = File.ReadAllText(Path.Combine(config, "controllers.ini"));
        Check(page.Capturing == null && text.Contains("[device 0300fa67c82d00000631000014017801]\n; XInput Controller #1 (xboxone)\nA = leftshoulder\n"), "A = leftshoulder in the pad's GUID section");
        Check(text.StartsWith("; the game's file\n[general]\nkeyboard_player = 1\n"), "the rest of the file kept");
        ((Button)page.Controls.Find("dpad_moves", true)[0]).PerformClick();
        text = File.ReadAllText(Path.Combine(config, "controllers.ini"));
        Check(text.Contains("dpad_moves = on"), "D-pad moves on for this pad");
        ((Button)page.Controls.Find("clear-A", true)[0]).PerformClick();
        text = File.ReadAllText(Path.Combine(config, "controllers.ini"));
        Check(!text.Contains("A = leftshoulder"), "A back to its default");
        pads.Held.Clear();
        pads.List.RemoveAt(0);
        typeof(RemapPage).GetField("nextScan", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(page, DateTime.MinValue);
        page.Poll();
        Check(page.Selected?.Id == 9, "unplugged: the next pad is chosen");
        page.Stop();
        Check(pads.Disposed, "SDL closed when the page goes");
        using var none = new RemapPage(repo, () => new FakePads { Problem = "no SDL3.dll" });
        none.Start();
        Check(none.StatusText == "no SDL3.dll" && none.Selected == null, "without SDL3.dll the page says why");
    }

    // GitHub's latest release, made up: the API's answer (a status, or no network) and github.com's releases/latest page,
    // whose redirect lands on the release's tag (or the release list when there is none).
    sealed class Releases : HttpMessageHandler
    {
        public HttpStatusCode Api = HttpStatusCode.OK;
        public string? ApiTag = "v0.4.1";
        public bool ApiDown, PageDown;
        public string PageLands = "/OpenSmash-Ultimate/OpenSmash-Ultimate/releases/tag/v0.4.1";
        public int PageAsked;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            if (request.RequestUri!.Host == "api.github.com")
            {
                if (ApiDown) throw new HttpRequestException("no network in tests");
                var answer = new HttpResponseMessage(Api);
                if (Api == HttpStatusCode.OK) answer.Content = new StringContent(ApiTag == null ? "{}" : "{\"tag_name\":\"" + ApiTag + "\"}");
                return Task.FromResult(answer);
            }
            PageAsked++;
            if (PageDown) throw new HttpRequestException("no network in tests");
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                RequestMessage = new HttpRequestMessage(request.Method, new Uri("https://github.com" + PageLands)),
            });
        }
    }

    // The update check's release channel and the verdict after an update (Discord "Can't update", 2026-11-18): GitHub not
    // answering is "not known", never main, and a finished update is not called unfinished for it.
    static async Task UpdateChannel()
    {
        async Task<ProjectSetup.LatestRelease> Ask(Releases r) { using var c = new HttpClient(r); return await ProjectSetup.LatestReleaseAsync(c); }
        var r = new Releases();
        var got = await Ask(r);
        Check(got.Tag == "v0.4.1" && got.Known && r.PageAsked == 0, "release channel: the API's tag");
        r = new Releases { Api = HttpStatusCode.NotFound };
        got = await Ask(r);
        Check(got.Tag == null && got.NoRelease && got.Known, "no release yet (the API's 404): main");
        r = new Releases { Api = HttpStatusCode.Forbidden };
        got = await Ask(r);
        Check(got.Tag == "v0.4.1" && r.PageAsked == 1, "the API's hourly limit: github.com's releases/latest page gives the tag");
        r = new Releases { ApiDown = true, PageLands = "/OpenSmash-Ultimate/OpenSmash-Ultimate/releases" };
        got = await Ask(r);
        Check(got.Tag == null && got.NoRelease, "no network to the API, the page lands on the release list: no release");
        r = new Releases { Api = HttpStatusCode.Forbidden, PageDown = true };
        got = await Ask(r);
        Check(!got.Known && got.Problem.Contains("403"), "neither answers: not known, with the API's reason (" + got.Problem + ")");
        r = new Releases { ApiTag = null, PageLands = "/OpenSmash-Ultimate/OpenSmash-Ultimate/releases/tag/v0.4.2" };
        got = await Ask(r);
        Check(got.Tag == "v0.4.2", "an API answer naming no release: the page");
        Check(ProjectSetup.ReleaseTagFromPath("/O/R/releases/tag/v1.0%2Bx") == "v1.0+x" && ProjectSetup.ReleaseTagFromPath("/O/R/releases") == null,
            "the tag of a releases/tag/ address");

        Check(LauncherForm.UpdateRefFor(new ProjectSetup.LatestRelease("v0.4.1", false, ""), out string name) == "v0.4.1" && name == "v0.4.1", "a tag is compared with");
        Check(LauncherForm.UpdateRefFor(new ProjectSetup.LatestRelease(null, true, ""), out name) == "main" && name == "main", "no release: main");
        bool threw = false;
        try { LauncherForm.UpdateRefFor(new ProjectSetup.LatestRelease(null, false, "GitHub answered 403"), out name); }
        catch (Exception e) { threw = LauncherForm.IsUnavailable(e) && e.Message.Contains("403"); }
        Check(threw, "not known: the check says it could not check, it never compares with main");

        string sha = new string('e', 40), other = new string('f', 40);
        string output = "launcher: OpenSmash.exe is now v0.4.1's.\r\n__OPENSMASH_SOURCE__ " + sha + "\r\nThe launcher was updated too: close OpenSmash and open it again.";
        Check(LauncherForm.InstalledCommit(output) == sha && LauncherForm.InstalledCommit("Nothing to update.") == "", "the updater's proof line");
        Check(LauncherForm.AfterUpdateVerdict("uptodate", false, "", sha) == LauncherForm.UpdateVerdict.Done, "checked up to date: done");
        Check(LauncherForm.AfterUpdateVerdict("error", true, output, sha) == LauncherForm.UpdateVerdict.Done, "GitHub not answering, the updater's proof matches the copy: done");
        Check(LauncherForm.AfterUpdateVerdict("available", false, output, sha) == LauncherForm.UpdateVerdict.Done, "the copy is what the updater installed: done");
        Check(LauncherForm.AfterUpdateVerdict("error", true, "Updated.", sha) == LauncherForm.UpdateVerdict.Unverified, "GitHub not answering, no proof: not known, not a failure");
        Check(LauncherForm.AfterUpdateVerdict("error", true, output, other) == LauncherForm.UpdateVerdict.Unverified, "proof of another commit, GitHub not answering: not known");
        Check(LauncherForm.AfterUpdateVerdict("available", false, "Updated.", sha) == LauncherForm.UpdateVerdict.NotFinished, "GitHub says there is still a newer one: did not finish");
    }

    static async Task Feed()
    {
        const string status = "{\"updated\":\"2026-11-16T13:57:58Z\",\"online\":5,\"rooms\":1,\"arenas\":0,\"open_rooms\":1,\"playing\":0,\"ranked\":78}";
        const string board = "{\"updated\":\"x\",\"ranked\":3,\"players\":[" +
            "{\"id\":\"bbb\",\"rank\":2,\"gsp\":1603,\"fighter\":0,\"fighters\":{}}," +
            "{\"id\":\"aaa\",\"rank\":1,\"gsp\":1658,\"fighter\":16,\"name\":\"Ann\\u0007ie\",\"avatar\":\"https://cdn.discordapp.com/embed/avatars/3.png\"}," +
            "{\"id\":\"ccc\",\"rank\":3,\"gsp\":1500,\"fighter\":23,\"avatar\":\"http://evil.example/x.png\"}]}";
        var s = CommunityFeed.ParseStatus(status)!;
        Check(s.Online == 5 && s.OpenRooms == 1 && s.Rooms == 1 && s.Ranked == 78, "status.json counts");
        Check(CommunityFeed.ParseStatus("<html>") == null && CommunityFeed.ParseStatus("[]") == null, "not a status: null");
        var b = CommunityFeed.ParseBoard(board, 2)!.Value;
        Check(b.Ranked == 3 && b.Top.Count == 2 && b.Top[0].Rank == 1 && b.Top[1].Id == "bbb", "top N by rank");
        Check(CommunityFeed.Who(b.Top[0]) == "Annie" && CommunityFeed.Who(b.Top[1]) == "Mario player", "a chosen name, else the main fighter");
        Check(b.Top[0].Avatar == "https://cdn.discordapp.com/embed/avatars/3.png" && b.Top[1].Avatar == "", "a row's Discord avatar, none when absent");
        Check(CommunityFeed.Mine(board, "ccc")?.Avatar == "" && CommunityFeed.AvatarUrl("https://cdn.discordapp.com.evil.example/a.png") == "", "an avatar not on Discord's CDN: initials");
        Check(CommunityFeed.Initials("Ann Bee") == "AB" && CommunityFeed.Initials("mario player") == "MP" && CommunityFeed.Initials("x") == "X"
              && CommunityFeed.Initials("  ") == "?", "initials when there is no picture");
        Check(CommunityFeed.Mine(board, "ccc")?.Gsp == 1500 && CommunityFeed.Mine(board, "zzz") == null, "the player's own row past the top");
        Check(CommunityFeed.ParseBoard("{\"players\":3}") == null, "not a leaderboard: null");
        var down = await CommunityFeed.Fetch(new HttpClient(new Refuse()), CancellationToken.None);
        Check(down.Status == null && down.Board == null, "lobby offline: nothing, no exception");
        var serve = new Serve(); serve.Pages["/status.json"] = status; serve.Pages["/leaderboard.json"] = board;
        var up = await CommunityFeed.Fetch(new HttpClient(serve), CancellationToken.None);
        Check(up.Status?.Online == 5 && up.Board == board, "lobby up: both pages");
        Check(CommunityFeed.Every == TimeSpan.FromSeconds(60), "asked again every 60 s");
    }

    // (by hand: LAUNCHER_TEST_RENDER=<folder> draws the home page's column and the remap page off screen into PNGs)
    static void Render(string repo, string outDir)
    {
        Directory.CreateDirectory(repo);
        Directory.CreateDirectory(outDir);
        using var form = new LauncherForm(repo, repo, Path.Combine(repo, "build"), preview: true);
        var type = typeof(LauncherForm);
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        void Set(string f, object? v) => type.GetField(f, flags)!.SetValue(form, v);
        Set("_lobby", CommunityFeed.ParseStatus("{\"online\":5,\"rooms\":1,\"open_rooms\":1,\"ranked\":78}"));
        var board = CommunityFeed.ParseBoard("{\"ranked\":78,\"players\":[" + string.Join(",", Enumerable.Range(1, 12).Select(i =>
            $"{{\"id\":\"p{i}\",\"rank\":{i},\"gsp\":{1700 - i * 13},\"fighter\":{i * 7 % 80}{(i % 3 == 0 ? ",\"name\":\"Player " + i + "\"" : "")}}}")) + "]}")!.Value;
        Set("_top", board.Top); Set("_ranked", board.Ranked); Set("_lobbyAsked", true); Set("_mine", board.Top[3]); Set("_discordHasApp", true);
        form.StartPosition = FormStartPosition.Manual;
        form.Location = new System.Drawing.Point(-20000, -20000);
        form.ClientSize = new System.Drawing.Size(1280, 720);
        form.Show(); Application.DoEvents();
        void Shot(string name, string view)
        {
            type.GetField("_paintedView", flags)!.SetValue(form, view);
            type.GetField("_viewAt", flags)!.SetValue(form, -10d);
            type.GetField("_shownAt", flags)!.SetValue(form, 0d);
            type.GetField("_heroAt", flags)!.SetValue(form, -10d);
            using var warm = new System.Drawing.Bitmap(1280, 720); form.DrawToBitmap(warm, new System.Drawing.Rectangle(0, 0, 1280, 720));
            Thread.Sleep(1200); Application.DoEvents();
            using var bmp = new System.Drawing.Bitmap(1280, 720); form.DrawToBitmap(bmp, new System.Drawing.Rectangle(0, 0, 1280, 720));
            bmp.Save(Path.Combine(outDir, name));
        }
        // (the server region: Auto with a pick kept, as after a start; the home page says whose numbers these are)
        Directory.CreateDirectory(Path.Combine(repo, "config"));
        File.WriteAllText(Path.Combine(repo, "config", "server_region_auto.txt"), "eu 24 1700000000\n");
        File.WriteAllText(Path.Combine(repo, "online_server.txt"), "auto\n");
        type.GetMethod("RegionView", flags)!.Invoke(form, null);
        Shot("home.png", "home");
        type.GetMethod("ReadSettings", flags)!.Invoke(form, null);
        Set("_view", "settings");
        Set("_settingsTab", Array.FindIndex((Array.ConvertAll((object[])type.GetField("_settingsTabs", flags)!.GetValue(form)!, t => (string)t.GetType().GetField("Name")!.GetValue(t)!)), n => n == "Online"));
        Shot("settings-online.png", "settings");
        // (Settings > Storage: the Save disk space row above the buttons, REBUILD SHADERS (#449) beside REFRESH - no two hit
        // areas may cross; the window keeps 16:9 and draws by its height, so one size says it for all)
        Set("_settingsTab", Array.FindIndex((Array.ConvertAll((object[])type.GetField("_settingsTabs", flags)!.GetValue(form)!, t => (string)t.GetType().GetField("Name")!.GetValue(t)!)), n => n == "Storage"));
        Set("_storageText", "dump: 17.2 GB on C:\\OpenSmash\\dump. build: 30.4 GB on C:\\OpenSmash\\build. A long status line, as the real one can be, wraps here.");
        Shot("settings-storage.png", "settings");
        var hits = ((System.Collections.IList)type.GetField("_hits", flags)!.GetValue(form)!).Cast<object>().Select(h =>
        {
            var ht = h.GetType();
            float F(string n) => (float)(ht.GetField(n)?.GetValue(h) ?? ht.GetProperty(n)!.GetValue(h)!);
            return (Id: (string)(ht.GetField("Id")?.GetValue(h) ?? ht.GetProperty("Id")!.GetValue(h)!), R: new System.Drawing.RectangleF(F("X"), F("Y"), F("W"), F("H")));
        }).Where(h => h.Id.StartsWith("storage:")).ToList();
        Console.WriteLine("Storage hits: " + string.Join("; ", hits.Select(h => $"{h.Id} {h.R.X:0},{h.R.Y:0} {h.R.Width:0}x{h.R.Height:0}")));
        Check(hits.Any(h => h.Id == "storage:save_disk") && hits.Any(h => h.Id == "storage:shaders"), "Storage: Save disk space and REBUILD SHADERS both drawn");
        for (int i = 0; i < hits.Count; i++)
            for (int j = i + 1; j < hits.Count; j++)
                Check(!hits[i].R.IntersectsWith(hits[j].R), $"Storage: {hits[i].Id} and {hits[j].Id} do not overlap");
        form.Close();
    }

    static void Presence(string repo)
    {
        string build = Path.Combine(repo, "build");
        Directory.CreateDirectory(build);
        Check(DiscordPresence.Start(build, "1234567890") == null, "no SDK beside the game: no presence, no failure");
        File.WriteAllText(Path.Combine(build, "discord_partner_sdk.dll"), "not a library");
        Check(DiscordPresence.Start(build, "1234567890") == null, "a broken SDK: no presence, no failure");
        Check(DiscordPresence.Start(build, "") == null, "no application id: none");
        Environment.SetEnvironmentVariable("SSBU_DISCORD", "0");
        Check(DiscordPresence.SdkPath(build) == null, "SSBU_DISCORD=0 turns it off");
        Environment.SetEnvironmentVariable("SSBU_DISCORD", null);
    }
}
