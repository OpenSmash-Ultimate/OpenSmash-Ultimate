using System.Reflection;
using OpenSmash;
using OpenSmash.Launcher;
using OpenSmash.Visuals;

// The settings page against the game's own files (ported from the WinForms launcher's test_launcher_settings.cs and
// test_launcher_localization.cs): the game's defaults, its lobby choices, only what was changed here is written.
static class SettingsChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }
    static string Root(string temporary, string name, bool migrated)
    {
        string root = Path.Combine(temporary, name);
        Directory.CreateDirectory(Path.Combine(root, "runtime"));
        Directory.CreateDirectory(Path.Combine(root, "tools"));
        File.WriteAllText(Path.Combine(root, "tools", "os_mods.py"), "");
        if (migrated) { Directory.CreateDirectory(Path.Combine(root, "config")); File.WriteAllText(Path.Combine(root, "config", "version.ini"), "version = 1"); }
        return root;
    }
    static string[] Files(string root) => Directory.GetFiles(root, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(root, f)).Order(StringComparer.Ordinal).ToArray();
    static object[] Settings(ClassicSurface s) => ((Array)typeof(ClassicSurface).GetField("_settings", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(s)!).Cast<object>().ToArray();
    static T Field<T>(object o, string name) => (T)o.GetType().GetField(name)!.GetValue(o)!;
    static object Setting(ClassicSurface s, string key) => Settings(s).Single(x => Field<string>(x, "Key") == key);

    public static void Run(string temporary)
    {
        Environment.SetEnvironmentVariable("SSBU_UI_LANGUAGE", null);
        Lobby();
        OnlineRules(temporary);
        FreshFolder(temporary);
        GameChangesKept(temporary);
        Language(temporary);
    }

    static void Lobby()
    {
        string pub = SettingDefaults.PublicLobby;
        foreach (var (line, lobby, back) in new[] { ("auto", "auto", "auto"), ("na", "auto", "auto"), ("eu", "auto", "auto"), (pub + " abc", "private", pub + " abc"),
                     ("10.0.0.2:39420", "custom", "10.0.0.2:39420"), ("lan", "lan", "lan"), ("off", "offline", "off"), ("", "offline", "off") })
        {
            var (l, code, server) = SettingDefaults.LobbyFromLine(line);
            Check(l == lobby && SettingDefaults.LineFromLobby(l, code, server) == back && SettingDefaults.Choices["lobby"].Contains(l), $"lobby line '{line}' reads as {lobby} and writes back '{back}'");
        }
    }

    static void FreshFolder(string temporary)
    {
        string root = Root(temporary, "settings-fresh", false);
        string[] before = Files(root);
        var window = new MainWindow([], root, enableBackgroundActions: false);
        var surface = window.Surface;
        foreach (var row in SettingDefaults.Table.Where(r => r.Key != "" && r.Key != "ui_language"))
            Check(surface.GetSetting(row.Key) == (row.Key == "renderer" ? SettingDefaults.Renderer(row.Default) : row.Default), $"fresh: {row.Key} shows the game's default {row.Default}");
        // (the official lobby is the launchers' default: an unset online_server.txt is written "auto", nothing else)
        Check(surface.GetSetting("lobby") == "auto" && surface.GetSetting("direct") == "off" && surface.GetSetting("code") == "" && surface.GetSetting("name") == "", "fresh: no online file is the official lobby, direct off");
        before = before.Append("online_server.txt").Order(StringComparer.Ordinal).ToArray();
        Check(File.ReadAllText(Path.Combine(root, "online_server.txt")).Trim() == "auto", "fresh: the official lobby's line written");
        Check(Field<string[]>(Setting(surface, "lobby"), "Values").SequenceEqual(SettingDefaults.Choices["lobby"]) && Field<string[]>(Setting(surface, "window"), "Values").SequenceEqual(SettingDefaults.Choices["window"]), "lists offer the game menu's choices");
        Check(surface.ValidateSettingsForPlay() && Files(root).SequenceEqual(before), "fresh: Play writes no file (" + string.Join(", ", Files(root).Except(before)) + ")");
        string? asked = null; surface.ActionRequested += a => asked = a;
        surface.Activate("set:" + Array.IndexOf(Settings(surface), Setting(surface, "remap")));
        Check(asked == "settings:remap", "Controllers' remap row asks for the remap page");
        window.Close();
    }

    static void GameChangesKept(string temporary)
    {
        string root = Root(temporary, "settings-game", true), config = Path.Combine(root, "config");
        File.WriteAllText(Path.Combine(config, "online_server.txt"), "auto\n");
        var window = new MainWindow([], root, enableBackgroundActions: false);
        var surface = window.Surface;
        Check(surface.GetSetting("lobby") == "auto" && surface.GetSetting("renderer") == "auto", "read before the game");
        // (the game's Esc menu changes its files while the launcher waits)
        File.WriteAllText(Path.Combine(config, "display.ini"), "window = borderless\nrenderer = vulkan\n");
        File.WriteAllText(Path.Combine(config, "online_server.txt"), "lan\n");
        File.WriteAllText(Path.Combine(config, "online_direct.txt"), "off\n");
        surface.ValidateSettingsForPlay();
        Check(File.ReadAllText(Path.Combine(config, "display.ini")) == "window = borderless\nrenderer = vulkan\n" && File.ReadAllText(Path.Combine(config, "online_server.txt")) == "lan\n", "the game's display.ini and lobby are kept");
        Check(!File.Exists(Path.Combine(config, "launcher.ini")) && !File.Exists(Path.Combine(root, "display.ini")), "nothing remembered that was not changed here; files stay under config/");
        window.Close();
        window = new MainWindow([], root, enableBackgroundActions: false); surface = window.Surface;
        Check(surface.GetSetting("window") == "borderless" && surface.GetSetting("renderer") == "vulkan" && surface.GetSetting("lobby") == "lan" && surface.GetSetting("direct") == "off", "next start shows the game's choices");
        surface.ChangeSetting("lobby", "auto");
        Check(File.ReadAllText(Path.Combine(config, "online_server.txt")).Trim() == "auto" && File.ReadAllText(Path.Combine(config, "display.ini")) == "window = borderless\nrenderer = vulkan\n", "picking the public lobby writes 'auto' and leaves display.ini alone");
        surface.ChangeSetting("lobby", "custom"); surface.ChangeSetting("server", "10.0.0.2:39420");
        Check(File.ReadAllText(Path.Combine(config, "online_server.txt")).Trim() == "10.0.0.2:39420" && File.ReadAllText(Path.Combine(config, "launcher.ini")).Contains("server = 10.0.0.2:39420"), "own server written; launcher.ini remembers it");
        surface.ChangeSetting("lobby", "private");
        Check(File.ReadAllText(Path.Combine(config, "online_server.txt")).Trim() == "10.0.0.2:39420" && !surface.ValidateSettingsForPlay(), "a private lobby without a code is not written");
        window.Close();
        window = new MainWindow([], root, enableBackgroundActions: false);
        Check(window.Surface.GetSetting("lobby") == "custom" && window.Surface.GetSetting("server") == "10.0.0.2:39420", "online_server.txt, not launcher.ini, is the lobby's truth");
        window.Close();
    }

    static void Language(string temporary)
    {
        string root = Root(temporary, "settings-language", true), file = Path.Combine(root, "config", "display.ini");
        File.WriteAllText(file, "; retained comment\nlanguage = ja\nwindow = borderless\n");
        var window = new MainWindow([], root, enableBackgroundActions: false);
        var surface = window.Surface;
        Check(UiLocale.Codes.Contains("zh-CN") && Field<string[]>(Setting(surface, "ui_language"), "Values").SequenceEqual(UiLocale.Codes), "language is selectable among the shared catalogs");
        surface.ChangeSetting("ui_language", "zh-CN");
        string saved = File.ReadAllText(file);
        Check(UiLocale.Language == "zh-CN" && UiLocale.Text("Settings") != "Settings", "launcher setting switches the language immediately");
        Check(saved.Contains("ui_language = zh-CN") && saved.Contains("language = ja") && saved.Contains("window = borderless") && saved.Contains("; retained comment"), "UI choice preserves the game language and other values");
        File.WriteAllText(file, saved.Replace("ui_language = zh-CN", "ui_language = en"));
        surface.ValidateSettingsForPlay();
        Check(File.ReadAllText(file).Contains("ui_language = en"), "launcher does not overwrite a newer in-game language choice");
        window.Close();
        UiLocale.Select("en");
    }

    // The Online rows (OpenSmash.Core.OnlineSettings, as main's launcher): the official lobby hides AND resets the lobby code,
    // the server of one's own and Direct connections (off), here and in launcher.ini's remembered copy; a server of one's own
    // shows them again, empty, Direct connections back on; nothing half-typed reaches online_server.txt; the server region
    // is Europe or North America East, saved, read back after a restart and the lobby played on.
    static void OnlineRules(string temporary)
    {
        string root = Root(temporary, "settings-online", true), config = Path.Combine(root, "config");
        string server = Path.Combine(config, "online_server.txt"), direct = Path.Combine(config, "online_direct.txt"), ini = Path.Combine(config, "launcher.ini");
        File.WriteAllText(server, "auto\n");
        File.WriteAllText(direct, "on\n");
        File.WriteAllText(ini, "[opensmash]\ncode = oldcode\nserver = my.host:39420\n");   // (an older launcher's leftovers)
        string IniOf(string key) => File.ReadAllLines(ini).Select(l => l.Split('=')).Where(p => p.Length == 2 && p[0].Trim() == key).Select(p => p[1].Trim()).FirstOrDefault() ?? "";
        var window = new MainWindow([], root, enableBackgroundActions: false);
        var s = window.Surface;
        s.Navigate("settings");
        bool On(string key) => (bool)typeof(ClassicSurface).GetMethod("SettingOn", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(s, new[] { Setting(s, key) })!;
        Check(s.GetSetting("lobby") == "auto" && s.GetSetting("code") == "" && s.GetSetting("server") == "" && s.GetSetting("direct") == "off",
              "official lobby: an old launcher.ini's code and server and an online_direct.txt 'on' are not shown");
        Check(!On("code") && !On("server") && !On("direct") && On("region") && On("name"), "official lobby: only the name and the server region are shown");
        Check(Field<string[]>(Setting(s, "region"), "Values").SequenceEqual(new[] { "eu", "na-east" }) && Field<string[]>(Setting(s, "region"), "Labels").SequenceEqual(new[] { "Europe", "North America East" }),
              "Server region: Europe and North America East");
        Check(File.ReadAllText(server) == "auto\n" && File.ReadAllText(direct) == "on\n", "reading writes nothing");
        s.ChangeSetting("lobby", "custom");
        Check(s.GetSetting("code") == "" && s.GetSetting("server") == "" && s.GetSetting("direct") == "on" && On("code") && On("server") && On("direct") && !On("region"),
              "your own server: code, server and direct connections shown again, empty, direct on");
        Check(File.ReadAllText(server) == "auto\n" && IniOf("code") == "" && IniOf("server") == "", "no made-up address written; launcher.ini's stale code and server reset");
        s.ChangeSetting("server", "my.host:39420");
        s.ChangeSetting("code", "abc");
        Check(File.ReadAllText(server).Trim() == "my.host:39420 abc", "your own server with its code written");
        s.ChangeSetting("lobby", "auto");
        Check(s.GetSetting("code") == "" && s.GetSetting("server") == "" && s.GetSetting("direct") == "off" && File.ReadAllText(server).Trim() == "auto"
              && File.ReadAllText(direct).Trim() == "off" && IniOf("code") == "" && IniOf("server") == "", "back to the official lobby: code and server reset everywhere, direct off");
        s.ChangeSetting("region", "na-east");
        Check(OpenSmash.Core.ServerRegion.Setting(root) == "na-east" && OpenSmash.Core.OfficialLobby.Current(root) == "na.lobby.opensmashultimate.com:39421", "North America East picked: saved and played on");
        window.Close();
        var again = new MainWindow([], root, enableBackgroundActions: false);   // (a restart)
        var s2 = again.Surface;
        s2.Navigate("settings");
        Check(s2.GetSetting("lobby") == "auto" && s2.GetSetting("region") == "na-east" && s2.GetSetting("code") == "", "after a restart: the official lobby, North America East, nothing stale");
        s2.ChangeSetting("region", "eu");
        Check(OpenSmash.Core.ServerRegion.Setting(root) == "eu" && OpenSmash.Core.OfficialLobby.Current(root) == OpenSmash.Core.ServerRegion.ById("eu").Lobby, "Europe picked: saved and played on");
        s2.ChangeSetting("lobby", "private");
        Check(File.ReadAllText(server).Trim() == "auto" && !s2.ValidateSettingsForPlay(), "private lobby without its code: not written, Play asks for it");
        s2.ChangeSetting("code", "friends1");
        Check(File.ReadAllText(server).Trim() == SettingDefaults.PublicLobby + " friends1", "private lobby with its code");
        s2.ChangeSetting("lobby", "lan");
        Check(File.ReadAllText(server).Trim() == "lan" && File.ReadAllText(direct).Trim() == "on", "LAN: direct connections back on");
        s2.ChangeSetting("lobby", "auto"); s2.ChangeSetting("lobby", "custom"); s2.ChangeSetting("lobby", "auto");
        Check(File.ReadAllText(server).Trim() == "auto" && s2.GetSetting("code") == "" && s2.GetSetting("server") == "" && File.ReadAllText(direct).Trim() == "off", "back and forth: the official lobby, consistent");
        again.Close();
    }}
