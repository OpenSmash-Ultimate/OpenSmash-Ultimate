using System.Drawing;
using System.Globalization;
using System.Text.Json;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    static (string Key, string Value)[] ThemeDefaults() =>
        [("theme_accent", "#FF3B30"), ("theme_background", "#050505"), ("theme_text", "#F4F4F2")];

    static bool ValidThemeColor(string value) => value.Length == 7 && value[0] == '#'
        && int.TryParse(value.AsSpan(1), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out _);

    bool _themePickerOpen;
    async Task PickThemeColor(string key, string label)
    {
        if (_themePickerOpen || Avalonia.Controls.TopLevel.GetTopLevel(this) is not Avalonia.Controls.Window owner) return;
        _themePickerOpen = true;
        try
        {
            var picker = new ThemeColorPicker(label, GetSetting(key)) { Icon = owner.Icon };
            string? color = await picker.ShowDialog<string?>(owner);
            if (color != null) ChangeSetting(key, color);
        }
        finally { _themePickerOpen = false; }
    }

    void ChangeThemeColor(string key, string value)
    {
        if (!ThemeDefaults().Any(item => item.Key == key)) return;
        if (!ValidThemeColor(value)) { Alert("CHECK YOUR COLOR", "Enter a color as #RRGGBB, for example #FF3B30."); return; }
        var colors = ThemeDefaults().ToDictionary(item => item.Key, item => GetSetting(item.Key));
        colors[key] = value.ToUpperInvariant();
        SaveThemeColors(new OpenSmash.Core.ThemePreset("Custom", colors["theme_accent"], colors["theme_background"], colors["theme_text"]));
    }

    void ResetTheme()
    {
        SaveThemeColors(OpenSmash.Core.ThemePresets.BuiltIns()[0], "builtin:OpenSmash");
    }

    Color _logoAccent, _logoText;
    void ApplyLogoTheme()
    {
        if (_logoAccent == ArtworkTheme.Accent && _logoText == ArtworkTheme.Text) return;
        using var stream = Avalonia.Platform.AssetLoader.Open(new Uri("avares://OpenSmash/Assets/emblem-white.png"));
        using var source = SkiaSharp.SKBitmap.Decode(stream);
        var pixels = source.Pixels;
        for (int i = 0; i < pixels.Length; i++)
        {
            var pixel = pixels[i];
            // Separate the original white mark from its red cross, keeping edge coverage.
            int white = Math.Min(pixel.Red, Math.Min(pixel.Green, pixel.Blue));
            int red = pixel.Red - white;
            byte Channel(byte text, byte accent) => (byte)((white * text + red * accent + 127) / 255);
            pixels[i] = new SkiaSharp.SKColor(Channel(ArtworkTheme.Text.R, ArtworkTheme.Accent.R),
                Channel(ArtworkTheme.Text.G, ArtworkTheme.Accent.G), Channel(ArtworkTheme.Text.B, ArtworkTheme.Accent.B), pixel.Alpha);
        }
        var themed = new SkiaSharp.SKBitmap(source.Width, source.Height, source.ColorType, source.AlphaType) { Pixels = pixels };
        _logo?.Dispose();
        _logo = new Bitmap(themed);
        _logoAccent = ArtworkTheme.Accent; _logoText = ArtworkTheme.Text;
        _themeWindowIcon = null;
    }

    void ApplyTheme()
    {
        Color Read(string key, string fallback) => Color.FromArgb(255,
            Color.FromArgb(int.Parse(_iniValues.GetValueOrDefault(key, fallback)[1..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)));
        ArtworkTheme.Accent = Read("theme_accent", "#FF3B30");
        ArtworkTheme.Bg = Read("theme_background", "#050505");
        ArtworkTheme.Text = Read("theme_text", "#F4F4F2");
        bool defaults = ThemeDefaults().All(item => _iniValues.GetValueOrDefault(item.Key, item.Value) == item.Value);
        ArtworkTheme.Bg2 = defaults ? Color.FromArgb(13,13,14) : ArtworkTheme.Mix(ArtworkTheme.Bg, ArtworkTheme.Text, .035f);
        ArtworkTheme.Bg3 = defaults ? Color.FromArgb(21,21,23) : ArtworkTheme.Mix(ArtworkTheme.Bg, ArtworkTheme.Text, .07f);
        ArtworkTheme.Muted = defaults ? Color.FromArgb(180,180,176) : ArtworkTheme.Mix(ArtworkTheme.Bg, ArtworkTheme.Text, .73f);
        ArtworkTheme.Dim = defaults ? Color.FromArgb(138,138,134) : ArtworkTheme.Mix(ArtworkTheme.Bg, ArtworkTheme.Text, .55f);
        ArtworkTheme.Accent2 = defaults ? Color.FromArgb(212,42,32) : ArtworkTheme.Mix(ArtworkTheme.Accent, ArtworkTheme.Bg, .2f);
        ApplyLogoTheme();
        ApplyWindowIcon();
        _bg?.Dispose(); _bg = null;
        _settingEdit.Background = Avalonia.Media.Brush.Parse(Hex(ArtworkTheme.Bg2));
        _settingEdit.Foreground = Avalonia.Media.Brush.Parse(Hex(ArtworkTheme.Text));
        if (_marketplaceReady) _ = MarketplaceScript(ThemeScript());
        InvalidateVisual();
    }

    static string Hex(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}";

    string ThemeScript()
    {
        var colors = new Dictionary<string,string>
        {
            ["--bg"] = Hex(ArtworkTheme.Bg), ["--bg-2"] = Hex(ArtworkTheme.Bg2), ["--bg-3"] = Hex(ArtworkTheme.Bg3),
            ["--text"] = Hex(ArtworkTheme.Text), ["--muted"] = Hex(ArtworkTheme.Muted), ["--dim"] = Hex(ArtworkTheme.Dim),
            ["--accent"] = Hex(ArtworkTheme.Accent), ["--accent2"] = Hex(ArtworkTheme.Accent2)
        };
        return "Object.entries(" + JsonSerializer.Serialize(colors) + ").forEach(([key,value])=>document.documentElement.style.setProperty(key,value));";
    }
}
