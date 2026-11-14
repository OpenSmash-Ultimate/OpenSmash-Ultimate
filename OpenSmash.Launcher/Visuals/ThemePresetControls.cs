using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using OpenSmash.Core;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    ThemePreset[] _savedThemes = [];
    string PresetsFile
    {
        get
        {
            string current = Path.Combine(Path.GetDirectoryName(Project!.ConfigFile("launcher.ini"))!, "launcher-themes.json");
            string legacy = Project.PathIn("launcher-themes.json");
            return !File.Exists(current) && File.Exists(legacy) ? legacy : current;
        }
    }

    void LoadThemePresets()
    {
        _savedThemes = [];
        try { _savedThemes = ThemePresets.Load(PresetsFile); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        { Alert("COULD NOT READ PRESETS", error.Message + " Your saved preset file has not been changed."); }
        string? saved = IniValue(Project!.ConfigFile("launcher.ini"), "theme", "preset");
        if (saved != null)
        {
            string id = Uri.UnescapeDataString(saved);
            _iniValues["theme_preset"] = id;
            if (FindThemePreset(id) is { } preset)
            {
                _iniValues["theme_accent"] = preset.Accent.ToUpperInvariant();
                _iniValues["theme_background"] = preset.Background.ToUpperInvariant();
                _iniValues["theme_text"] = preset.Text.ToUpperInvariant();
                ApplyTheme();
            }
        }
        RefreshThemePresets();
    }

    ThemePreset? FindThemePreset(string id) => ThemePresets.BuiltIns().FirstOrDefault(p => id == "builtin:" + p.Name)
        ?? _savedThemes.FirstOrDefault(p => id == "saved:" + p.Name);

    void RefreshThemePresets()
    {
        var setting = _settings.First(s => s.Key == "theme_preset");
        var builtIns = ThemePresets.BuiltIns();
        setting.Values = new[] { "custom" }.Concat(builtIns.Select(p => "builtin:" + p.Name)).Concat(_savedThemes.Select(p => "saved:" + p.Name)).ToArray();
        setting.Labels = new[] { "Custom colors" }.Concat(builtIns.Select(p => p.Name)).Concat(_savedThemes.Select(p => p.Name + " (saved)")).ToArray();
        var matches = builtIns.Select(p => (Id: "builtin:" + p.Name, Preset: p)).Concat(_savedThemes.Select(p => (Id: "saved:" + p.Name, Preset: p)));
        string selected = _iniValues.GetValueOrDefault("theme_preset", "");
        if (selected == "custom" || matches.Any(p => p.Id == selected)) return;
        _iniValues["theme_preset"] = matches.FirstOrDefault(p => p.Preset.Accent.Equals(GetSetting("theme_accent"), StringComparison.OrdinalIgnoreCase)
            && p.Preset.Background.Equals(GetSetting("theme_background"), StringComparison.OrdinalIgnoreCase)
            && p.Preset.Text.Equals(GetSetting("theme_text"), StringComparison.OrdinalIgnoreCase)).Id ?? "custom";
    }

    void ApplyThemePreset(string id)
    {
        if (FindThemePreset(id) is { } preset) SaveThemeColors(preset, id);
    }

    void SaveThemeColors(ThemePreset preset, string selection = "custom")
    {
        string path = Project!.ConfigFile("launcher.ini"), temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            if (File.Exists(path)) File.Copy(path, temporary);
            foreach (var (key, value) in new[] { ("theme_accent", preset.Accent), ("theme_background", preset.Background), ("theme_text", preset.Text) })
                SetIni(temporary, "theme", key, value.ToUpperInvariant());
            SetIni(temporary, "theme", "preset", Uri.EscapeDataString(selection));
            if (File.Exists(path)) File.Copy(path, path + ".theme-backup", true);
            File.Move(temporary, path, true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { Alert("COULD NOT SAVE", error.Message); return; }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
        _iniValues["theme_accent"] = preset.Accent.ToUpperInvariant();
        _iniValues["theme_background"] = preset.Background.ToUpperInvariant();
        _iniValues["theme_text"] = preset.Text.ToUpperInvariant();
        _iniValues["theme_preset"] = selection;
        ApplyTheme(); RefreshThemePresets();
    }

    bool _presetDialogOpen;
    async Task NameThemePreset()
    {
        if (_presetDialogOpen || TopLevel.GetTopLevel(this) is not Window owner) return;
        _presetDialogOpen = true;
        try
        {
            var name = new TextBox { MaxLength = 40, PlaceholderText = "My theme" };
            Avalonia.Automation.AutomationProperties.SetName(name, "Preset name");
            var save = new Button { Content = "Save", IsDefault = true, IsEnabled = false };
            var cancel = new Button { Content = "Cancel", IsCancel = true };
            var dialog = new Window { Icon = owner.Icon, Title = "Save theme preset", Width = 380, Height = 210, CanResize = false,
                WindowStartupLocation = WindowStartupLocation.CenterOwner, Background = Avalonia.Media.Brush.Parse("#151517"), Foreground = Brushes.White };
            dialog.Content = new StackPanel { Margin = new Thickness(24), Spacing = 12, Children =
            {
                new TextBlock { Text = "Name your current colors" }, name,
                new StackPanel { Orientation = Avalonia.Layout.Orientation.Horizontal, Spacing = 12, Children = { cancel, save } }
            }};
            name.PropertyChanged += (_, e) => { if (e.Property == TextBox.TextProperty) save.IsEnabled = !string.IsNullOrWhiteSpace(name.Text) && !name.Text.Any(char.IsControl); };
            save.Click += (_, _) => dialog.Close(name.Text!.Trim());
            cancel.Click += (_, _) => dialog.Close(null);
            dialog.Opened += (_, _) => name.Focus();
            string? result = await dialog.ShowDialog<string?>(owner);
            if (result == null) return;
            var preset = new ThemePreset(result, GetSetting("theme_accent"), GetSetting("theme_background"), GetSetting("theme_text"));
            void Store(bool replace)
            {
                try
                {
                    ThemePresets.Save(PresetsFile, preset, replace);
                    _savedThemes = ThemePresets.Load(PresetsFile);
                    SaveThemeColors(preset, "saved:" + preset.Name);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException or InvalidOperationException)
                { Alert("COULD NOT SAVE PRESET", error.Message); }

            }
            if (ThemePresets.Load(PresetsFile).Any(p => p.Name.Equals(result, StringComparison.OrdinalIgnoreCase)))
                Confirm("REPLACE PRESET?", "Replace the saved theme ‘" + result + "’ with these colors?", () => Store(true));
            else Store(false);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        { Alert("COULD NOT SAVE PRESET", error.Message); }
        finally { _presetDialogOpen = false; }
    }
}
