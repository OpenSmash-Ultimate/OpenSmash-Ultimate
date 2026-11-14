using System.Text.Json;
using System.Text.RegularExpressions;

namespace OpenSmash.Core;

public sealed record ThemePreset(string Name, string Accent, string Background, string Text)
{
    public bool IsValid => !string.IsNullOrWhiteSpace(Name) && Name.Length <= 40 && !Name.Any(char.IsControl)
        && new[] { Accent, Background, Text }.All(color => color != null && color.Length == 7 && Regex.IsMatch(color, "^#[0-9a-fA-F]{6}$"));
}

public static class ThemePresets
{
    public static ThemePreset[] BuiltIns() =>
    [
        new("OpenSmash", "#FF3B30", "#050505", "#F4F4F2"),
        new("Catppuccin Mocha", "#CBA6F7", "#1E1E2E", "#CDD6F4"),
        new("Dracula", "#BD93F9", "#282A36", "#F8F8F2"),
        new("Gruvbox", "#FABD2F", "#282828", "#EBDBB2"),
        new("Nord", "#88C0D0", "#2E3440", "#ECEFF4"),
        new("Solarized Light", "#268BD2", "#FDF6E3", "#073642"),
        new("Tokyo Night", "#7AA2F7", "#1A1B26", "#C0CAF5")
    ];

    public static ThemePreset[] Load(string path)
    {
        if (!File.Exists(path)) return [];
        var presets = JsonSerializer.Deserialize<ThemePreset[]>(File.ReadAllText(path))
            ?? throw new InvalidDataException("The saved theme presets are empty or invalid.");
        if (presets.Any(p => p == null || !p.IsValid) || presets.Select(p => p.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != presets.Length)
            throw new InvalidDataException("The saved theme presets contain invalid colors or duplicate names.");
        return presets.OrderBy(p => p.Name, StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public static void Save(string path, ThemePreset preset, bool replace = false)
    {
        if (!preset.IsValid) throw new ArgumentException("Use a name of 1–40 characters and valid hex colors.");
        var presets = Load(path).ToList();
        int index = presets.FindIndex(p => p.Name.Equals(preset.Name, StringComparison.OrdinalIgnoreCase));
        if (index >= 0 && !replace) throw new InvalidOperationException("A preset with that name already exists.");
        if (index >= 0) presets[index] = preset; else presets.Add(preset);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, JsonSerializer.Serialize(presets.OrderBy(p => p.Name, StringComparer.OrdinalIgnoreCase), new JsonSerializerOptions { WriteIndented = true }));
            if (File.Exists(path)) File.Copy(path, path + ".bak", true);
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
