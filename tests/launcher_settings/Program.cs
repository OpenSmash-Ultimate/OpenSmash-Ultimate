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
            FreshFolder(Path.Combine(root, "fresh"));
            GameChangesKept(Path.Combine(root, "game"));
            Profiles();
            Remap(Path.Combine(root, "remap"));
            Feed().GetAwaiter().GetResult();
            Presence(Path.Combine(root, "presence"));
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

    // online_server.txt's lines as the game's settings menu reads and writes them (runtime/gpu_menu.c net_load / net_save_server)
    static void Lobby()
    {
        string pub = SettingDefaults.PublicLobby;
        var cases = new (string Line, string Lobby, string Back)[]
        {
            ("", "offline", "off"), ("off", "offline", "off"), ("auto", "auto", "auto"), ("na", "auto", "auto"), ("eu", "auto", "auto"),
            ("smash.recomps.wtf:39421", "auto", "auto"), ("smash.recomps.wtf:39421 mine", "private", pub + " mine"),
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
        Check(SettingDefaults.Renderer("d3d11") == "auto" && SettingDefaults.Renderer("DX11") == "direct3d11" && SettingDefaults.Renderer("vk") == "vulkan", "renderer names as runtime/renderer.h");
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
            Check(values["lobby"] == "offline" && values["direct"] == "on" && values["name"] == "", "fresh: no online files is offline, direct on");
            call("PersistSettings");
            Check(Files(repo).Length == 0, "fresh: closing writes no file (" + string.Join(", ", Files(repo)) + ")");
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

    static async Task Feed()
    {
        const string status = "{\"updated\":\"2026-11-16T13:57:58Z\",\"online\":5,\"rooms\":1,\"arenas\":0,\"open_rooms\":1,\"playing\":0,\"ranked\":78}";
        const string board = "{\"updated\":\"x\",\"ranked\":3,\"players\":[" +
            "{\"id\":\"bbb\",\"rank\":2,\"gsp\":1603,\"fighter\":0,\"fighters\":{}}," +
            "{\"id\":\"aaa\",\"rank\":1,\"gsp\":1658,\"fighter\":16,\"name\":\"Ann\\u0007ie\"}," +
            "{\"id\":\"ccc\",\"rank\":3,\"gsp\":1500,\"fighter\":23}]}";
        var s = CommunityFeed.ParseStatus(status)!;
        Check(s.Online == 5 && s.OpenRooms == 1 && s.Rooms == 1 && s.Ranked == 78, "status.json counts");
        Check(CommunityFeed.ParseStatus("<html>") == null && CommunityFeed.ParseStatus("[]") == null, "not a status: null");
        var b = CommunityFeed.ParseBoard(board, 2)!.Value;
        Check(b.Ranked == 3 && b.Top.Count == 2 && b.Top[0].Rank == 1 && b.Top[1].Id == "bbb", "top N by rank");
        Check(CommunityFeed.Who(b.Top[0]) == "Annie" && CommunityFeed.Who(b.Top[1]) == "Mario player", "a chosen name, else the main fighter");
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
        Shot("home.png", "home");
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
