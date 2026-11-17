using System.Reflection;
using OpenSmash;
using OpenSmash.Core;
using OpenSmash.Visuals;
using SkiaSharp;

static class ThemePresetChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }
    public static void Run(string temporary)
    {
        string root = Path.Combine(temporary, "preset-checks");
        Directory.CreateDirectory(Path.Combine(root,"tools"));
        Directory.CreateDirectory(Path.Combine(root,"runtime"));
        File.WriteAllText(Path.Combine(root,"tools","os_mods.py"), "");
        string store = Path.Combine(root, "launcher-themes.json");
        var builtIns = ThemePresets.BuiltIns();
        Check(builtIns.Length == 7 && builtIns.All(p => p.IsValid), "seven valid built-in theme palettes are available");
        var custom = new ThemePreset("My colors", "#123456", "#112233", "#EEEEFF");
        ThemePresets.Save(store,custom);
        Check(ThemePresets.Load(store).Single()==custom, "named custom preset survives save and reload");
        string before = File.ReadAllText(store);
        bool refused = false;
        try { ThemePresets.Save(store,custom with { Name = "MY COLORS" }); }
        catch (InvalidOperationException) { refused = true; }
        Check(refused && File.ReadAllText(store)==before, "duplicate names cannot silently overwrite a saved preset");
        ThemePresets.Save(store,custom with { Accent = "#ABCDEF" },replace:true);
        Check(File.ReadAllText(store+".bak")==before && ThemePresets.Load(store).Single().Accent=="#ABCDEF", "confirmed preset replacement keeps a recoverable backup");
        refused = false;
        try { ThemePresets.Save(store,custom with { Accent = "#123456\n" }); }
        catch (ArgumentException) { refused = true; }
        Check(refused, "preset color validation rejects trailing data");
        string damaged = Path.Combine(root,"damaged-presets.json");
        File.WriteAllText(damaged,"not JSON"); refused=false;
        try { ThemePresets.Save(damaged,custom); }
        catch (System.Text.Json.JsonException) { refused = true; }
        Check(refused && File.ReadAllText(damaged)=="not JSON", "saving refuses to overwrite unreadable preset data");
        File.WriteAllText(Path.Combine(root,"launcher.ini"),"[other]\nkeep = yes\n");
        var window = new MainWindow([],root,enableBackgroundActions:false);
        window.Show();
        var surface = window.Surface;
        var icon = window.Icon;
        var settings = (Array)typeof(ClassicSurface).GetField("_settings",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        int presetIndex = Array.FindIndex(settings.Cast<object>().ToArray(),setting => (string)setting.GetType().GetField("Key")!.GetValue(setting)! == "theme_preset");
        var presetSetting = settings.GetValue(presetIndex)!;
        var options = (string[])presetSetting.GetType().GetField("Values")!.GetValue(presetSetting)!;
        surface.Activate("set:" + presetIndex + ":opt:" + Array.IndexOf(options,"builtin:Nord"));
        Check(surface.GetSetting("theme_accent")=="#88C0D0" && surface.GetSetting("theme_background")=="#2E3440" && surface.GetSetting("theme_text")=="#ECEFF4", "choosing a built-in preset updates all three colors");
        Check(surface.GetSetting("theme_preset")=="builtin:Nord", "preset dropdown tracks the selected palette");
        Check(window.Icon != null && !ReferenceEquals(window.Icon,icon), "running window icon updates with the palette");
        byte[] bytes = (byte[])typeof(ClassicSurface).GetMethod("MakeThemeIcon",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(surface,null)!;
        using (var reader = new BinaryReader(new MemoryStream(bytes)))
        {
            Check(reader.ReadUInt16()==0 && reader.ReadUInt16()==1 && reader.ReadUInt16()==6, "Windows icon contains six resolutions");
            reader.BaseStream.Position=6+5*16+8;
            int length=reader.ReadInt32(), offset=reader.ReadInt32();
            using var bitmap = SKBitmap.Decode(bytes.AsSpan(offset,length).ToArray());
            Check(bitmap.Width==256 && bitmap.GetPixel(85,50)==new SKColor(0x88,0xC0,0xD0), "Windows icon pixels use the themed logo accent");
        }
        string config = Path.Combine(root,"launcher.ini");
        Check(File.ReadAllText(config).Contains("keep = yes") && File.Exists(config+".theme-backup"), "applying presets preserves unrelated settings and backs up the previous config");
        surface.ChangeSetting("theme_preset","saved:My colors");
        Check(surface.GetSetting("theme_accent")=="#ABCDEF" && surface.GetSetting("theme_preset")=="saved:My colors", "saved presets can be selected from the same dropdown");
        surface.ChangeSetting("theme_accent","#010203");
        Check(surface.GetSetting("theme_preset")=="custom", "editing a preset color switches the selector to custom");
        surface.ChangeSetting("theme_preset","builtin:Solarized Light");
        var reopened = new MainWindow([],root,enableBackgroundActions:false);
        reopened.Show();
        Check(reopened.Surface.GetSetting("theme_preset")=="builtin:Solarized Light" && reopened.Icon != null, "preset and window icon restore on reopening");
        reopened.Close(); window.Close();
    }
}
