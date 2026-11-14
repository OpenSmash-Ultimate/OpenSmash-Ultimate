using System.Text;
using System.Text.Json;
using Avalonia.Platform.Storage;

namespace OpenSmash;

public sealed partial class MainWindow
{
    static JsonElement ModResult(string output)
    {
        string? line = output.Split('\n').LastOrDefault(l => l.StartsWith("RESULT "));
        if (line == null) throw new InvalidOperationException(output);
        using var doc = JsonDocument.Parse(line[7..]);
        if (!doc.RootElement.GetProperty("ok").GetBoolean()) throw new InvalidOperationException(output);
        return doc.RootElement.Clone();
    }

    void CheckModIdle()
    {
        if (Surface.GameRunning || Surface.IsInstalling || Surface.ModsRunning || Surface.OperationRunning)
            throw new InvalidOperationException("Close the game and wait for the current operation to finish.");
    }

    async Task CheckMods()
    {
        CheckModIdle(); Surface.SetOperationLabel("CHECKING MODS"); Surface.OperationRunning = true;
        try
        {
            var conflicts = ModResult(await Python("os_mods.py", "conflicts"));
            var updates = ModResult(await Python("os_mods.py", "updates"));
            var text = new StringBuilder();
            var list = conflicts.GetProperty("conflicts");
            int count = list.GetArrayLength();
            text.AppendLine(count == 0 ? "No file is changed by two turned-on mods." : $"{count} file(s) changed by more than one turned-on mod (the game reads the last one's):");
            foreach (var item in list.EnumerateArray().Take(12))
                text.AppendLine($"  {item.GetProperty("path").GetString()}: {string.Join(", ", item.GetProperty("mods").EnumerateArray().Select(m => m.GetString()))} -> {item.GetProperty("wins").GetString()}");
            if (count > 12) text.AppendLine($"  ... and {count - 12} more");
            text.AppendLine();
            var available = updates.GetProperty("updates").EnumerateArray().Select(item =>
                (gb: item.GetProperty("gamebanana").GetString() ?? "", file: item.GetProperty("file_id").ToString(), title: item.GetProperty("title").GetString() ?? "")).ToList();
            text.AppendLine(available.Count == 0 ? "Every GameBanana mod is up to date." : $"{available.Count} mod(s) with a newer file on GameBanana:");
            foreach (var item in available) text.AppendLine("  " + item.title);
            Surface.ShowModCheck(text.ToString().Trim(), available);
        }
        finally { Surface.OperationRunning = false; Surface.InvalidateVisual(); }
    }

    async Task AddLocalMod()
    {
        CheckModIdle();
        // (asked before the folder picker, so the choice is seen up front)
        string choice=await Surface.ChooseModImport();if(choice=="cancel"||_closed)return;
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = choice=="link" ? "Choose a mod folder to add as a link" : "Choose a mod folder to copy", AllowMultiple = false });
        if (folders.FirstOrDefault()?.TryGetLocalPath() is not string path) return;
        CheckModIdle();
        string name=Path.GetFileName(Path.TrimEndingDirectorySeparator(path));
        string destination=RequireProject().PathIn("mods",name);
        if(Directory.Exists(destination)||File.Exists(destination))throw new InvalidOperationException("A mod called "+name+" is already installed.");
        string sourceRoot=Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        if(Path.GetFullPath(destination).StartsWith(sourceRoot+Path.DirectorySeparatorChar,OperatingSystem.IsWindows()?StringComparison.OrdinalIgnoreCase:StringComparison.Ordinal))
            throw new IOException("Choose the mod folder itself, outside this project's mods folder.");
        Directory.CreateDirectory(RequireProject().PathIn("mods"));
        if(choice=="link")Directory.CreateSymbolicLink(destination,path);
        else
        {
            Surface.SetOperationLabel("COPYING MOD");Surface.OperationRunning=true;
            try{await Task.Run(()=>CopyModFolder(path,destination));}
            finally{Surface.OperationRunning=false;}
        }
        await ModCommand("enable",[name]);
    }
    static void CopyModFolder(string source,string destination)
    {
        Directory.CreateDirectory(destination);
        foreach(string file in Directory.GetFiles(source))File.Copy(file,Path.Combine(destination,Path.GetFileName(file)));
        foreach(string folder in Directory.GetDirectories(source))
        {
            if((File.GetAttributes(folder)&FileAttributes.ReparsePoint)!=0)
                throw new IOException("The mod contains a linked subfolder. Use Add as link to keep its layout.");
            CopyModFolder(folder,Path.Combine(destination,Path.GetFileName(folder)));
        }
    }

    async Task ShareModList(bool export)
    {
        CheckModIdle();
        var json = new FilePickerFileType("Mod list (*.json)") { Patterns = ["*.json"] };
        string? path;
        if (export)
        {
            var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
                { Title = "Share mods", SuggestedFileName = "opensmash-mods.json", DefaultExtension = "json", FileTypeChoices = [json] });
            path = file?.TryGetLocalPath();
        }
        else
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
                { Title = "Install shared mods", AllowMultiple = false, FileTypeFilter = [json] });
            path = files.FirstOrDefault()?.TryGetLocalPath();
        }
        if (path == null) return;
        CheckModIdle(); Surface.SetOperationLabel(export?"SAVING THE LIST":"INSTALLING THE LIST"); Surface.OperationRunning = true;
        try
        {
            var result = ModResult(await Python("os_mods.py", export ? "export-list" : "import-list", path));
            if(!export&&result.TryGetProperty("installed",out var installed))new OpenSmash.Core.ModChanges(RequireProject()).Remember(installed.EnumerateArray().Select(n=>n.GetString()!));
            await RefreshMods();
            if (export) Surface.Alert("MOD LIST", "Saved: " + path);
            else
            {
                int Count(string key) => result.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.Array ? value.GetArrayLength() : 0;
                string missing = result.TryGetProperty("missing", out var items) ? string.Join(", ", items.EnumerateArray().Select(v => v.GetString())) : "";
                Surface.Alert("MOD LIST", $"{Count("installed")} installed, {Count("already")} already here" + (missing.Length > 0 ? $", not on GameBanana (ask your friend for them): {missing}" : "."));
            }
        }
        finally { Surface.OperationRunning = false; Surface.InvalidateVisual(); }
    }
}
