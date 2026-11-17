using Avalonia.Headless;
using OpenSmash;
using OpenSmash.Core;
using OpenSmash.Launcher;
using OpenSmash.Visuals;

// The controller remap page (main's test_launcher_settings.cs Profiles/Remap), with simulated pads: no controller needed.
static class RemapChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }

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

    public static void Run(string root)
    {
        Profiles();
        State(Path.Combine(root, "remap"));
        Page(root);
        using (var missing = new SdlPadSource(Path.Combine(root, "nowhere", "SDL3.dll"), root))
            Check(missing.Problem != "" && missing.Pads().Count == 0, "remap: no SDL3: a reason, no pads");
        using (var missing2 = new Sdl2PadSource(Path.Combine(root, "nowhere", "libSDL2-2.0.so.0"), root))
            Check(missing2.Problem != "" && missing2.Pads().Count == 0, "remap: no SDL2: a reason, no pads");
        Check(Sdl2PadSource.TypeName(2) == "xboxone" && Sdl2PadSource.TypeName(7) == "ps5" && Sdl2PadSource.TypeName(5) == "switchpro" && Sdl2PadSource.TypeName(0) == "standard",
              "remap: SDL2 types named as hle_pad.c's sdl2_type");
        Check(PadSources.GuidText(0x0807060504030201, 0x100f0e0d0c0b0a09) == "0102030405060708090a0b0c0d0e0f10", "remap: GUID text as SDL_GUIDToString writes it");
        // (a real SDL, when one is given: it loads and lists the pads, none needed)
        string? sdl = Environment.GetEnvironmentVariable("LAUNCHER_TEST_SDL_DIR");
        if (!string.IsNullOrEmpty(sdl))
        {
            using var real = PadSources.ForProject(sdl);
            Check(real.Problem == "", "remap: the game's SDL loads: " + real.Problem);
            var list = real.Pads();
            Console.WriteLine($"SDL lists {list.Count} pad(s): " + string.Join("; ", list.Select(p => $"{p.Name} ({p.Type}, GUID {p.Guid})")));
            foreach (var p in list) Check(p.Guid.Length == 32, "remap: GUID as SDL writes it");
        }
    }

    static void Profiles()
    {
        string ini = "[general]\nkeyboard_player = 1\n[device default]\ndpad_moves = off\n" +
                     "[device XInput  Controller #1]   ; the menu's\nA = leftshoulder\n[device xboxone]\nB = north\nrumble = 50 # half\n";
        var all = PadProfile.Sections(ini);
        Check(all.ContainsKey("device XInput Controller #1"), "remap: a section named with a '#' is whole");
        Check(all["device xboxone"]["rumble"] == "50", "remap: '# half' is a comment");
        Check(PadProfile.Effective(ini, "guid", "XInput Controller #1", "xboxone", "A") == ("leftshoulder", "device XInput Controller #1"), "remap: name's section for this pad");
        Check(PadProfile.Effective(ini, "guid", "XInput Controller #1", "xboxone", "B") == ("north", "device xboxone"), "remap: type's section under it");
        Check(PadProfile.Effective(ini, "guid", "XInput Controller #2", "xboxone", "A").From == "", "remap: another port's name: default A");
        string g = "0300fa67c82d00000631000014017801";
        string w = PadProfile.Set(ini, PadProfile.SectionFor(g), "A", "south", "XInput Controller #1 (xboxone)");
        Check(w.StartsWith(ini) && w.EndsWith("\n[device " + g + "]\n; XInput Controller #1 (xboxone)\nA = south\n"), "remap: a new GUID section at the end, with the pad's name");
        Check(PadProfile.Effective(w, g, "XInput Controller #1", "xboxone", "A") == ("south", "device " + g), "remap: GUID's section over the name's");
        w = PadProfile.Set(w, PadProfile.SectionFor(g), "dpad_moves", "on");
        w = PadProfile.Set(w, PadProfile.SectionFor(g), "A", "east");
        Check(w.Contains("[device " + g + "]\n; XInput Controller #1 (xboxone)\nA = east\ndpad_moves = on\n"), "remap: keys replaced in place, added after the last");
        w = PadProfile.Set(w, PadProfile.SectionFor(g), "A", null);
        Check(!w.Contains("A = east") && PadProfile.Effective(w, g, "XInput Controller #1", "xboxone", "A").Value == "leftshoulder", "remap: removed: the name's again");
        Check(PadProfile.Set("x = 1\r\n", "device a", "A", "south") == "x = 1\r\n\r\n[device a]\r\nA = south\r\n", "remap: CRLF files stay CRLF");
        Check(PadProfile.NewlyPressed(new HashSet<string> { "south" }, new HashSet<string> { "south", "righttrigger" }) == "righttrigger", "remap: a newly pressed control");
        Check(PadProfile.NewlyPressed(new HashSet<string> { "south" }, new HashSet<string> { "south" }) == null, "remap: a held button does not count");
        Check(PadProfile.Buttons.All(PadProfile.Positional.ContainsKey) && PadProfile.Positional.Values.All(PadProfile.Sources.Contains), "remap: every button has a default control");
    }

    static void State(string repo)
    {
        string config = Path.Combine(repo, "config"), file = Path.Combine(config, "controllers.ini");
        Directory.CreateDirectory(config);
        File.WriteAllText(file, "; the game's file\n[general]\nkeyboard_player = 1\n");
        var pads = new FakePads();
        pads.List.Add(new PadDevice(7, "XInput Controller #1", "xboxone", "0300fa67c82d00000631000014017801"));
        pads.List.Add(new PadDevice(9, "DualSense Wireless Controller", "ps5", "050000004c050000e60c000000016800"));
        var now = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        var page = new RemapState(file, () => pads, () => now);
        page.Start();
        Check(page.Selected?.Id == 7 && page.Pads.Count == 2, "remap: pads listed, the first chosen");
        page.Choose(1);
        Check(page.Selected?.Id == 9, "remap: choosing the DualSense");
        page.Choose(0);
        pads.Held.Add("south");   // (held when SET is pressed: not the answer)
        page.BeginCapture("A");
        Check(page.Capturing == "A", "remap: waiting for a press");
        page.Poll();
        Check(page.Capturing == "A", "remap: a button already held is not taken");
        pads.Held.Add("leftshoulder");
        page.Poll();
        string text = File.ReadAllText(file);
        Check(page.Capturing == null && text.Contains("[device 0300fa67c82d00000631000014017801]\n; XInput Controller #1 (xboxone)\nA = leftshoulder\n"), "remap: A = leftshoulder in the pad's GUID section");
        Check(text.StartsWith("; the game's file\n[general]\nkeyboard_player = 1\n"), "remap: the rest of the file kept");
        pads.Held.Clear();
        foreach (var (button, control) in new[] { ("B", "east"), ("ZR", "rightshoulder") })
        {
            page.BeginCapture(button); pads.Held.Add(control); page.Poll(); pads.Held.Clear();
        }
        text = File.ReadAllText(file);
        Check(text.Contains("A = leftshoulder\nB = east\nZR = rightshoulder\n"), "remap: a capture per console button, in the same section");
        page.BeginCapture("X");
        now = now.AddSeconds(9); page.Poll();
        Check(page.Capturing == null && page.Status.StartsWith("Nothing was pressed"), "remap: a capture gives up after 8 s");
        page.BeginCapture("X");
        Check(page.CancelCapture() && page.Capturing == null, "remap: Esc cancels a capture");
        page.Toggle("dpad_moves");
        Check(File.ReadAllText(file).Contains("dpad_moves = on") && page.Effective("dpad_moves") == "on", "remap: D-pad moves on for this pad");
        page.Write("A", null);
        Check(!File.ReadAllText(file).Contains("A = leftshoulder"), "remap: A back to its default");
        // layering: default < type < name < GUID, as the game reads it
        File.AppendAllText(file, "\n[device default]\nY = south\nX = south\n[device xboxone]\nY = east\nX = east\n[device XInput  Controller #1]\nX = north\n");
        Check(PadProfile.Effective(page.IniText(), "0300fa67c82d00000631000014017801", "XInput Controller #1", "xboxone", "Y").Value == "east" &&
              PadProfile.Effective(page.IniText(), "0300fa67c82d00000631000014017801", "XInput Controller #1", "xboxone", "X").Value == "north" &&
              page.Effective("B") == "east", "remap: sections layer default < type < name < GUID");
        page.ResetPad();
        text = File.ReadAllText(file);
        Check(!text.Contains("ZR = rightshoulder") && !text.Contains("dpad_moves = on") && text.Contains("[device xboxone]\nY = east\nX = east\n") &&
              PadProfile.Effective(text, "0300fa67c82d00000631000014017801", "XInput Controller #1", "xboxone", "ZR").From == "", "remap: reset clears only the pad's own keys");
        pads.List.RemoveAt(0);
        now = now.AddSeconds(2); page.Poll();
        Check(page.Selected?.Id == 9, "remap: unplugged: the next pad is chosen");
        page.Stop();
        Check(pads.Disposed, "remap: SDL closed when the page goes");
        var none = new RemapState(file, () => new FakePads { Problem = "no SDL3.dll" });
        none.Start();
        Check(none.Status == "no SDL3.dll" && none.Selected == null, "remap: without SDL the page says why");
    }

    static void Page(string root)
    {
        var project = new Project(root, Project.CurrentPlatform);
        bool built = project.IsBuilt;
        if (!built) { Directory.CreateDirectory(Path.GetDirectoryName(project.GameExecutable)!); File.WriteAllText(project.GameExecutable, "synthetic test fixture"); }
        string ini = project.ConfigFile("controllers.ini");
        string? saved = File.Exists(ini) ? File.ReadAllText(ini) : null;
        var window = new MainWindow([], root, enableBackgroundActions: false);
        try
        {
            window.Show(); window.Width = 1280; window.Height = 720;
            var surface = window.Surface; surface.FrozenTime = 5;
            var pads = new FakePads();
            pads.List.Add(new PadDevice(3, "Pro Controller #2", "switchpro", "030000007e0500000920000000006800"));
            surface.PadSourceFactory = () => pads;
            surface.Navigate("settings");
            void Action(string id) => ((Task)typeof(MainWindow).GetMethod("Action", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(window, [id])!).GetAwaiter().GetResult();
            Action("settings:remap");
            Avalonia.Threading.Dispatcher.UIThread.RunJobs();
            Check(surface.CurrentView == "remap" && surface.Remap?.Selected?.Name == "Pro Controller #2", "remap: settings:remap opens the page with the pads listed");
            window.CaptureRenderedFrame();
            surface.Activate("remap:set:PLUS");
            pads.Held.Add("start");
            surface.Remap!.Poll();
            Check(File.ReadAllText(ini).Contains("[device 030000007e0500000920000000006800]\n; Pro Controller #2 (switchpro)\nPLUS = start\n"), "remap: the page writes [device <GUID>] into config/controllers.ini");
            window.CaptureRenderedFrame();
            surface.Activate("nav:back");
            Check(surface.CurrentView == "settings" && surface.Remap == null && pads.Disposed, "remap: back returns to settings and closes SDL");
            Action("settings:remap");
            surface.Activate("remap:set:A");
            surface.Focus();
            window.KeyPressQwerty(Avalonia.Input.PhysicalKey.Escape, Avalonia.Input.RawInputModifiers.None);
            Check(surface.CurrentView == "remap" && surface.Remap?.Capturing == null, "remap: Esc cancels the capture first");
            window.KeyPressQwerty(Avalonia.Input.PhysicalKey.Escape, Avalonia.Input.RawInputModifiers.None);
            Check(surface.CurrentView == "settings", "remap: Esc again goes back to settings");
        }
        finally
        {
            window.Close();
            if (saved == null) File.Delete(ini); else File.WriteAllText(ini, saved);
            if (!built) File.Delete(project.GameExecutable);
        }
    }
}
