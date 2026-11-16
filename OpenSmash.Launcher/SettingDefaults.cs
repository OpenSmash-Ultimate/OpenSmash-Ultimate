using System;
using System.Collections.Generic;
using System.Linq;

namespace OpenSmash.Launcher
{
    // Every setting the launcher's Settings page shows: where the game keeps it, and the value the game itself uses when
    // the file or the key is missing (its default). The launcher shows a missing value as this default and writes only
    // what the player changes in it, so a fresh download shows what the game will do and writes nothing.
    // tests/test_launcher_defaults.py reads this table and the runtime's own defaults (runtime/gpu_display.c's
    // default display.ini, runtime/hle_pad.c's controllers.ini, runtime/gpu_menu.c's lists, runtime/ipc_audio.c) and
    // fails when they differ: change both together.
    static class SettingDefaults
    {
        // (file under config\, its [section] ("" none), key ("" the file's first line), the game's default)
        public static readonly (string File, string Section, string Key, string Default)[] Table =
        {
            ("display.ini", "", "ui_language", "auto"),
            ("display.ini", "", "window", "windowed"),
            ("display.ini", "", "width", "1280"),
            ("display.ini", "", "height", "720"),
            ("display.ini", "", "pacing", "clock"),
            ("display.ini", "", "render_scale", "1"),
            ("display.ini", "", "gpu", "auto"),
            ("display.ini", "", "renderer", "auto"),   // (Windows: Vulkan first on AMD, NVIDIA and Intel cards, else Direct3D 11)
            ("display.ini", "", "hide_cursor", "1"),
            ("display.ini", "", "overlay", "1"),
            ("display.ini", "", "volume", "100"),
            ("controllers.ini", "general", "keyboard_player", "1"),
            ("controllers.ini", "general", "background", "1"),
            ("online_server.txt", "", "", "off"),      // (no file: offline; the download comes with "auto")
            ("online_direct.txt", "", "", "on"),
            ("online_name.txt", "", "", ""),
        };

        // The values each list offers, as the game's settings menu (Esc) cycles through them.
        public static readonly Dictionary<string, string[]> Choices = new()
        {
            ["window"] = new[] { "windowed", "borderless", "fullscreen" },
            ["window_size"] = new[] { "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" },
            ["pacing"] = new[] { "clock", "vrr", "display" },
            ["render_scale"] = new[] { "1", "auto", "1.5", "2", "3" },
            ["renderer"] = new[] { "auto", "direct3d11", "vulkan" },
            ["lobby"] = new[] { "auto", "private", "custom", "lan", "offline" },
        };

        public static string Of(string key)
        {
            foreach (var row in Table)
                if (row.Key == key) return row.Default;
            throw new KeyNotFoundException(key);
        }

        public static string OfFile(string file)
        {
            foreach (var row in Table)
                if (row.File == file && row.Key == "") return row.Default;
            throw new KeyNotFoundException(file);
        }

        public const string PublicLobby = "opensmash-lobby.duckdns.org:39421";   // (runtime/gpu_menu.c's PUBLIC_LOBBY: the one public lobby)
        const string OldLobby = "smash.recomps.wtf";   // (the North American lobby, gone: its lines mean the public one)

        // online_server.txt's line read as the game's settings menu reads it (runtime/gpu_menu.c net_load):
        // the lobby choice, the lobby code and a server of your own.
        public static (string Lobby, string Code, string Server) LobbyFromLine(string line)
        {
            var words = (line ?? "").Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            string host = words.Length > 0 ? words[0] : "", code = words.Length > 1 ? words[1] : "";
            if (host == "" || host.Equals("off", StringComparison.OrdinalIgnoreCase)) return ("offline", code, "");
            if (host.Equals("lan", StringComparison.OrdinalIgnoreCase)) return ("lan", code, "");
            if (host.Equals("auto", StringComparison.OrdinalIgnoreCase) || host.Equals("na", StringComparison.OrdinalIgnoreCase) ||
                host.Equals("eu", StringComparison.OrdinalIgnoreCase) || host.StartsWith(OldLobby, StringComparison.OrdinalIgnoreCase) ||
                host.Equals(PublicLobby, StringComparison.OrdinalIgnoreCase))
                return (code == "" ? "auto" : "private", code, "");
            return ("custom", code, host);
        }

        // A server address that is the public lobby (its own, or the gone North American one's, with or without the
        // port): "Your own server" with it is the Public lobby (issue #389: an older launcher.ini's custom server
        // smash.recomps.wtf:39421 was written back at Play).
        public static bool MeansPublicLobby(string? host)
        {
            host = (host ?? "").Trim();
            return host.StartsWith(OldLobby, StringComparison.OrdinalIgnoreCase) || host.Equals(PublicLobby, StringComparison.OrdinalIgnoreCase) ||
                   host.Equals(PublicLobby.Split(':')[0], StringComparison.OrdinalIgnoreCase);
        }

        // online_server.txt's line names the gone North American lobby (smash.recomps.wtf): to be written again as "auto".
        public static bool IsOldLobbyLine(string? line)
        {
            var words = (line ?? "").Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            return words.Length > 0 && words[0].StartsWith(OldLobby, StringComparison.OrdinalIgnoreCase);
        }

        // The line the game's settings menu writes for a choice (runtime/gpu_menu.c net_save_server).
        public static string LineFromLobby(string lobby, string code, string server)
        {
            code = (code ?? "").Replace(" ", "");
            server = (server ?? "").Trim();
            if (lobby == "custom" && MeansPublicLobby(server)) lobby = code == "" ? "auto" : "private";
            return lobby switch
            {
                "offline" => "off",
                "lan" => "lan",
                "private" => PublicLobby + " " + (code == "" ? "friends" : code),
                "custom" => (server == "" ? "127.0.0.1:39420" : server) + (code == "" ? "" : " " + code),
                _ => "auto",   // (the one public lobby, in Europe: runtime/net_relay.c)
            };
        }

        // The renderer as the launcher shows it (runtime/renderer.h's renderer_parse on Windows).
        public static string Renderer(string? v) => (v ?? "").Trim().ToLowerInvariant() switch
        {
            "vulkan" or "vk" => "vulkan",
            "direct3d11" or "direct3d" or "dx11" => "direct3d11",
            _ => "auto",
        };
    }
}
