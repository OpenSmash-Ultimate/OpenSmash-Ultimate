using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Forms;
using OpenSmash.Core;

namespace OpenSmash.Launcher
{
    // Settings > Theme, as OpenSmash.Avalonia's (Visuals/LauncherTheme.cs, Visuals/ThemePresetControls.cs): the accent,
    // background and text colors, the built-in palettes (OpenSmash.Avalonia/Core/ThemePresets.cs, linked into this
    // project) and the player's own saved ones. The same files as that launcher's, so both show the same colors:
    // launcher.ini [theme] theme_accent / theme_background / theme_text / preset, and config\launcher-themes.json.
    partial class LauncherForm
    {
        static readonly string[] ThemeColorKeys = { "theme_accent", "theme_background", "theme_text" };

        static (string Key, string Value)[] ThemeDefaults() =>
            new[] { ("theme_accent", "#FF3B30"), ("theme_background", "#050505"), ("theme_text", "#F4F4F2") };

        static bool ValidThemeColor(string? value) => value != null && value.Length == 7 && value[0] == '#'
            && int.TryParse(value.AsSpan(1), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out _);

        ThemePreset[] _savedThemes = Array.Empty<ThemePreset>();
        Image? _logoSource;
        Color _logoAccent, _logoText;

        string LauncherIni => GameFolder.ConfigFile(_repo, "launcher.ini");

        string PresetsFile
        {
            get
            {
                string current = Path.Combine(Path.GetDirectoryName(LauncherIni)!, "launcher-themes.json");
                string legacy = Path.Combine(_repo, "launcher-themes.json");
                return !File.Exists(current) && File.Exists(legacy) ? legacy : current;
            }
        }

        // At start (before the first paint) and whenever the Settings page reads its files: the colors launcher.ini keeps.
        void LoadTheme()
        {
            foreach (var (key, fallback) in ThemeDefaults())
            {
                string? value = null;
                try { value = IniValue(LauncherIni, "theme", key); } catch { }
                _iniValues[key] = ValidThemeColor(value) ? value!.ToUpperInvariant() : fallback;
            }
            _savedThemes = Array.Empty<ThemePreset>();
            try { _savedThemes = ThemePresets.Load(PresetsFile); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
            { System.Diagnostics.Trace.WriteLine("theme presets: " + error.Message); }
            string? saved = null;
            try { saved = IniValue(LauncherIni, "theme", "preset"); } catch { }
            if (saved != null)
            {
                string id = Uri.UnescapeDataString(saved);
                _iniValues["theme_preset"] = id;
                if (FindThemePreset(id) is { } preset)
                {
                    _iniValues["theme_accent"] = preset.Accent.ToUpperInvariant();
                    _iniValues["theme_background"] = preset.Background.ToUpperInvariant();
                    _iniValues["theme_text"] = preset.Text.ToUpperInvariant();
                }
            }
            RefreshThemePresets();
            ApplyTheme();
        }

        ThemePreset? FindThemePreset(string id) => ThemePresets.BuiltIns().FirstOrDefault(p => id == "builtin:" + p.Name)
            ?? _savedThemes.FirstOrDefault(p => id == "saved:" + p.Name);

        // The preset list: custom, the built-in palettes, the saved ones; the current colors' own entry picked.
        void RefreshThemePresets()
        {
            var setting = _settings?.FirstOrDefault(s => s.Key == "theme_preset");
            var builtIns = ThemePresets.BuiltIns();
            if (setting != null)
            {
                setting.Values = new[] { "custom" }.Concat(builtIns.Select(p => "builtin:" + p.Name)).Concat(_savedThemes.Select(p => "saved:" + p.Name)).ToArray();
                setting.Labels = new[] { "Custom colors" }.Concat(builtIns.Select(p => p.Name)).Concat(_savedThemes.Select(p => p.Name + " (saved)")).ToArray();
            }
            foreach (string key in ThemeColorKeys)
            {
                var row = _settings?.FirstOrDefault(s => s.Key == key);
                if (row != null) row.Labels = new[] { _iniValues.GetValueOrDefault(key, "") };
            }
            var matches = builtIns.Select(p => (Id: "builtin:" + p.Name, Preset: p)).Concat(_savedThemes.Select(p => (Id: "saved:" + p.Name, Preset: p)));
            string selected = _iniValues.GetValueOrDefault("theme_preset", "");
            if (selected == "custom" || matches.Any(p => p.Id == selected)) return;
            string Get(string key) => _iniValues.GetValueOrDefault(key, "");
            _iniValues["theme_preset"] = matches.FirstOrDefault(p => p.Preset.Accent.Equals(Get("theme_accent"), StringComparison.OrdinalIgnoreCase)
                && p.Preset.Background.Equals(Get("theme_background"), StringComparison.OrdinalIgnoreCase)
                && p.Preset.Text.Equals(Get("theme_text"), StringComparison.OrdinalIgnoreCase)).Id ?? "custom";
        }

        // The rows of the Theme tab that act on a click instead of a list (the colors, save, reset): true when handled.
        bool ThemeAction(Setting set)
        {
            switch (set.Key)
            {
                case "theme_accent": case "theme_background": case "theme_text":
                    PickThemeColor(set.Key, set.Label); return true;
                case "theme_save": NameThemePreset(); return true;
                case "theme_reset": SaveThemeColors(ThemePresets.BuiltIns()[0], "builtin:" + ThemePresets.BuiltIns()[0].Name); return true;
            }
            return false;
        }

        // A choice in the preset list (WriteSetting): its colors written and shown.
        void ApplyThemePreset(string id)
        {
            if (FindThemePreset(id) is { } preset) SaveThemeColors(preset, id);
            else { _iniValues["theme_preset"] = "custom"; Invalidate(); }
        }

        void PickThemeColor(string key, string label)
        {
            using var dialog = new ColorDialog { FullOpen = true, AnyColor = true };
            string now = _iniValues.GetValueOrDefault(key, ThemeDefaults().First(d => d.Key == key).Value);
            try { dialog.Color = ColorTranslator.FromHtml(now); } catch { }
            dialog.CustomColors = ThemeColorKeys.Select(k => _iniValues.GetValueOrDefault(k, "#000000"))
                .Where(ValidThemeColor).Select(h => { var k = ColorTranslator.FromHtml(h); return k.B << 16 | k.G << 8 | k.R; }).ToArray();
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            string hex = Hex(dialog.Color);
            var colors = ThemeDefaults().ToDictionary(d => d.Key, d => _iniValues.GetValueOrDefault(d.Key, d.Value));
            colors[key] = hex;
            SaveThemeColors(new ThemePreset("Custom", colors["theme_accent"], colors["theme_background"], colors["theme_text"]));
        }

        void SaveThemeColors(ThemePreset preset, string selection = "custom")
        {
            string path = LauncherIni, temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
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
            { UiLocale.Message(UiLocale.Format("Could not save the theme: {0}", error.Message), "OpenSmash"); return; }
            finally { try { if (File.Exists(temporary)) File.Delete(temporary); } catch { } }
            _iniValues["theme_accent"] = preset.Accent.ToUpperInvariant();
            _iniValues["theme_background"] = preset.Background.ToUpperInvariant();
            _iniValues["theme_text"] = preset.Text.ToUpperInvariant();
            _iniValues["theme_preset"] = selection;
            _settingsStamp = SettingsFilesStamp();   // (written here: the page does not read it back as an outside change)
            RefreshThemePresets();
            ApplyTheme();
        }

        void NameThemePreset()
        {
            string? name = AskPresetName();
            if (name == null) return;
            string Get(string key) => _iniValues.GetValueOrDefault(key, "");
            var preset = new ThemePreset(name, Get("theme_accent"), Get("theme_background"), Get("theme_text"));
            try
            {
                bool exists = ThemePresets.Load(PresetsFile).Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
                if (exists && UiLocale.Message(this, UiLocale.Format("Replace the saved theme ‘{0}’ with these colors?", name), "OpenSmash",
                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                ThemePresets.Save(PresetsFile, preset, exists);
                _savedThemes = ThemePresets.Load(PresetsFile);
                SaveThemeColors(preset, "saved:" + preset.Name);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException or InvalidDataException)
            { UiLocale.Message(UiLocale.Format("Could not save the preset: {0}", error.Message), "OpenSmash"); }
        }

        string? AskPresetName()
        {
            using var dialog = new Form
            {
                Text = UiLocale.Text("Save theme preset"), FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent,
                MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false, BackColor = Theme.Bg2, ForeColor = Theme.Text,
                ClientSize = new Size(380, 150), Icon = Icon, Font = new Font("Segoe UI", 10f)
            };
            var label = new Label { Text = UiLocale.Text("Name your current colors"), AutoSize = true, Location = new Point(20, 18), ForeColor = Theme.Text };
            var box = new TextBox { MaxLength = 40, PlaceholderText = "My theme", Location = new Point(20, 46), Width = 340,
                BackColor = Theme.Bg3, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle };
            var save = new Button { Text = UiLocale.Text("Save"), DialogResult = DialogResult.OK, Enabled = false, Location = new Point(276, 100), Width = 84,
                FlatStyle = FlatStyle.Flat, BackColor = Theme.Accent, ForeColor = Theme.Bg };
            var cancel = new Button { Text = UiLocale.Text("Cancel"), DialogResult = DialogResult.Cancel, Location = new Point(180, 100), Width = 84,
                FlatStyle = FlatStyle.Flat, ForeColor = Theme.Text };
            box.TextChanged += (_, _) => save.Enabled = !string.IsNullOrWhiteSpace(box.Text) && !box.Text.Any(char.IsControl);
            dialog.Controls.AddRange(new Control[] { label, box, cancel, save });
            dialog.AcceptButton = save;
            dialog.CancelButton = cancel;
            return dialog.ShowDialog(this) == DialogResult.OK ? box.Text.Trim() : null;
        }

        // The colors in use: Theme's palette (the shades between them mixed as OpenSmash.Avalonia mixes them), the window,
        // the emblem, the mod browser's page.
        void ApplyTheme()
        {
            Color Read(string key, string fallback)
            {
                string v = _iniValues.GetValueOrDefault(key, fallback);
                if (!ValidThemeColor(v)) v = fallback;
                return Color.FromArgb(255, Color.FromArgb(int.Parse(v.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture)));
            }
            Color oldBg = Theme.Bg;
            bool defaults = ThemeDefaults().All(d => string.Equals(_iniValues.GetValueOrDefault(d.Key, d.Value), d.Value, StringComparison.OrdinalIgnoreCase));
            Theme.SetPalette(Read("theme_accent", "#FF3B30"), Read("theme_background", "#050505"), Read("theme_text", "#F4F4F2"), defaults);
            BackColor = Theme.Bg;
            foreach (Control c in Controls)
                if (c.BackColor.ToArgb() == oldBg.ToArgb()) c.BackColor = Theme.Bg;
            if (_settingEdit != null) { _settingEdit.BackColor = defaults ? Color.FromArgb(18, 18, 20) : Theme.Bg2; _settingEdit.ForeColor = Theme.Text; }
            ApplyLogoTheme();
            ClearScaleCaches();
            _slashCache?.Dispose(); _slashCache = null;
            _ = PushTheme();
            Invalidate(true);
        }

        // The white emblem with its red cross: the mark in the text color, the cross in the accent (OpenSmash.Avalonia's way).
        void ApplyLogoTheme()
        {
            if (_logoSource == null && _logo != null) _logoSource = _logo;
            if (_logoSource == null || (_logoAccent == Theme.Accent && _logoText == Theme.Text && _logo != _logoSource)) return;
            try
            {
                using var source = new Bitmap(_logoSource);
                var rect = new Rectangle(0, 0, source.Width, source.Height);
                var themed = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb);
                using (var g = Graphics.FromImage(themed)) g.DrawImage(source, rect);
                var data = themed.LockBits(rect, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
                try
                {
                    var px = new byte[data.Stride * data.Height];
                    Marshal.Copy(data.Scan0, px, 0, px.Length);
                    Color text = Theme.Text, accent = Theme.Accent;
                    for (int i = 0; i + 3 < px.Length; i += 4)
                    {
                        int b = px[i], gr = px[i + 1], r = px[i + 2];
                        int white = Math.Min(r, Math.Min(gr, b)), red = r - white;
                        byte Ch(byte t, byte a) => (byte)Math.Min(255, (white * t + red * a + 127) / 255);
                        px[i] = Ch(text.B, accent.B); px[i + 1] = Ch(text.G, accent.G); px[i + 2] = Ch(text.R, accent.R);
                    }
                    Marshal.Copy(px, 0, data.Scan0, px.Length);
                }
                finally { themed.UnlockBits(data); }
                if (_logo != null && _logo != _logoSource) _logo.Dispose();
                _logo = themed;
                _logoAccent = Theme.Accent; _logoText = Theme.Text;
            }
            catch (Exception error) { System.Diagnostics.Trace.WriteLine("emblem theme: " + error.Message); }
        }

        static Color? ColorTranslatorTry(string hex) =>
            ValidThemeColor(hex) ? Color.FromArgb(255, Color.FromArgb(int.Parse(hex.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture))) : null;

        static string Hex(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}";

        string ThemeScript()
        {
            var colors = new Dictionary<string, string>
            {
                ["--bg"] = Hex(Theme.Bg), ["--bg-2"] = Hex(Theme.Bg2), ["--bg-3"] = Hex(Theme.Bg3),
                ["--text"] = Hex(Theme.Text), ["--muted"] = Hex(Theme.Muted), ["--dim"] = Hex(Theme.Dim),
                ["--accent"] = Hex(Theme.Accent), ["--accent2"] = Hex(Theme.Accent2)
            };
            return "Object.entries(" + JsonSerializer.Serialize(colors) + ").forEach(([key,value])=>document.documentElement.style.setProperty(key,value));";
        }

        async Task PushTheme()
        {
            var core = _marketplaceWebView?.CoreWebView2;
            if (core == null) return;
            try { await core.ExecuteScriptAsync(ThemeScript()); } catch { }
        }
    }
}
