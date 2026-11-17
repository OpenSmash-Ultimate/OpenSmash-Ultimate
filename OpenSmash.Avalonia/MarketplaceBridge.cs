using System.Text.Json;
using System.Text.RegularExpressions;
using Avalonia.Threading;
using OpenSmash.Core;

namespace OpenSmash;

public sealed partial class MainWindow
{
    async Task MarketplaceMessage(string message)
    {
        try
        {
            using var doc = JsonDocument.Parse(message);
            var root = doc.RootElement;
            string Get(string key) => root.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
            string type = Get("type"), id = Get("modId"), name = Get("name");
            switch (type)
            {
                case "ready": Surface.MarketplaceLoaded(); await PushMarketplaceState(); return;
                case "escape": Surface.Navigate("mods"); return;
                case "detail": return;
                case "open_external": OpenExternal(Get("url")); return;
            }
            if (type is not ("install" or "remove" or "install_component" or "install_plugin")) return;
            string[] arguments;
            string script = "os_mods.py";
            if (type is "install" or "remove")
            {
                if (!Regex.IsMatch(id, @"^\d{1,12}$")) return;
                string file = Get("fileId");
                if (file.Length > 0 && !Regex.IsMatch(file, @"^\d{1,12}$")) return;
                arguments = type == "remove" ? ["remove-id", id] : file.Length > 0 ? ["install", id, file] : ["install", id];
            }
            else
            {
                string component = Get("component");
                id = type == "install_component" ? "component:" + component : "plugin:" + name;
                string plugin = component == "arcropolis" ? "libarcropolis.nro" : "";
                if (component == "skyline")
                {
                    // The native runtime already supplies Skyline's loader APIs.
                    await MarketplaceStatus(id, "done", "Skyline is provided by the native game runtime.", 100);
                    await PushMarketplaceState(); return;
                }
                if (type == "install_plugin")
                {
                    if (!Uri.TryCreate(Get("url"), UriKind.Absolute, out var uri) || uri.Scheme != "https" || !uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase)) return;
                    string repo = string.Join('/', uri.AbsolutePath.Split('/', StringSplitOptions.RemoveEmptyEntries).Take(2)).ToLowerInvariant();
                    plugin = repo switch { "hdr-development/smashline" => "libsmashline_plugin.nro", "ultimate-research/nro-hook-plugin" => "libnro_hook.nro", "raytwo/arcropolis" => "libarcropolis.nro", "csharpm7/lib_paramconfig" => "native", _ => "" };
                    if (plugin == "native") { await MarketplaceStatus(id, "done", "ParamConfig is provided by the native game runtime.", 100); return; }
                }
                if (plugin.Length == 0) { await MarketplaceStatus(id, "error", "This plugin is not in the project's pinned release list.", 0); return; }
                script = "std_plugins.py";
                arguments = ["fetch", "--tree", RequireProject().Root, plugin];
            }
            if (_checkingChanges || _playPreflight || Surface.HasPrompt || Surface.GameRunning || Surface.IsInstalling || Surface.ModsRunning || Surface.OperationRunning)
            { await MarketplaceStatus(id, "error", "Close the game and wait for the current operation to finish.", 0); return; }
            Surface.SetOperationLabel(type=="remove"?"REMOVING MOD":"INSTALLING "+name.ToUpperInvariant());
            Surface.OperationRunning = true;
            try
            {
                await MarketplaceStatus(id, "downloading", type == "remove" ? "Removing " + name : "Installing " + name, 0);
                string result = await Python(script, arguments, line =>
                {
                    if (!line.StartsWith("LOG ")) return;
                    string text = line[4..].Trim();
                    var percent = Regex.Match(text, @"^percent (\d+)$");
                    int progress = percent.Success ? Math.Clamp(int.Parse(percent.Groups[1].Value), 0, 100) : 92;
                    Dispatcher.UIThread.Post(async () => await MarketplaceStatus(id, progress < 85 ? "downloading" : "installing", percent.Success ? "Installing " + name : text, progress));
                });
                if (script == "os_mods.py")
                {
                    string? line = result.Split('\n').LastOrDefault(l => l.StartsWith("RESULT "));
                    if (line == null) throw new InvalidOperationException(result);
                    using var response = JsonDocument.Parse(line[7..]);
                    if (!response.RootElement.GetProperty("ok").GetBoolean()) throw new InvalidOperationException(result);
                    if(type=="install"&&response.RootElement.TryGetProperty("names",out var added))
                        new ModChanges(RequireProject()).Remember(added.EnumerateArray().Select(n=>n.GetString()!));
                }
                await PushMarketplaceState();
                await MarketplaceStatus(id, "done", name + (type == "remove" ? " removed." : " installed."), 100);
            }
            catch (Exception error) { await MarketplaceStatus(id, "error", error.Message, 0); }
            finally { Surface.OperationRunning = false; Surface.InvalidateVisual(); }
        }
        catch (JsonException) { /* Malformed web messages must not close the launcher. */ }
        catch (Exception error) { Surface.Alert("MOD BROWSER", error.Message); }
    }

    static void OpenExternal(string url)
    {
        if (Uri.TryCreate(url, UriKind.Absolute, out var uri) && uri.Scheme is "http" or "https") Commands.OpenPath(uri.AbsoluteUri);
    }

    async Task PushMarketplaceState()
    {
        await RefreshMods();
        var p = RequireProject();
        string folder = p.PathIn("skyline", "plugins");
        var files = Directory.Exists(folder) ? Directory.GetFiles(folder, "*.nro").Select(Path.GetFileNameWithoutExtension) : [];
        var plugins = files.Append("libparam_config").Distinct().ToArray();
        var inventory = new { moddingReady = plugins.Contains("libarcropolis"), plugins, mods = Surface.InstalledModFolders, loaders = new { skyline = p.IsBuilt, arcropolis = plugins.Contains("libarcropolis") } };
        await Surface.MarketplaceScript($"window.__osm && (window.__osm.setInstalled({JsonSerializer.Serialize(Surface.InstalledModIds)}), window.__osm.setInventory({JsonSerializer.Serialize(inventory)}));");
    }

    Task MarketplaceStatus(string modId, string state, string message, double percent) =>
        Surface.MarketplaceScript($"window.__osm && window.__osm.status({JsonSerializer.Serialize(new { modId, state, message, percent })});");
}
