using System.Reflection;
using OpenSmash;
using OpenSmash.Core;
using OpenSmash.Visuals;

static class ThemeChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }
    static SkiaSharp.SKColor LogoPixel(ClassicSurface surface, int x, int y)
    {
        var logo = typeof(ClassicSurface).GetField("_logo", BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        var bitmap = (SkiaSharp.SKBitmap)logo.GetType().GetProperty("Native")!.GetValue(logo)!;
        return bitmap.GetPixel(x,y);
    }

    static void PickerChecks(MainWindow owner)
    {
        var assembly = typeof(ClassicSurface).Assembly;
        var type = assembly.GetType("OpenSmash.Visuals.ThemeColorPicker")!;
        var picker = (Avalonia.Controls.Window)Activator.CreateInstance(type,["Accent color", "#FF3B30"])!;
        T Field<T>(string name) => (T)type.GetField(name,BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(picker)!;
        var hex = Field<Avalonia.Controls.TextBox>("_hex");
        var wheel = Field<Avalonia.Controls.Control>("_wheel");
        var brightness = Field<Avalonia.Controls.Slider>("_brightness");
        var apply = Field<Avalonia.Controls.Button>("_apply");
        owner.Show();
        picker.Show(owner);
        Avalonia.Threading.Dispatcher.UIThread.RunJobs();
        Check(hex.Text == "#FF3B30" && wheel.IsEffectivelyVisible, "color picker opens with a wheel and real hex textbox");
        hex.Text = "#00FF00";
        Check((Avalonia.Media.Color)wheel.GetType().GetProperty("SelectedColor")!.GetValue(wheel)! == Avalonia.Media.Colors.Lime,
            "typed hex updates wheel selection");
        brightness.Value = .5;
        Check(hex.Text == "#008000", "brightness slider updates hex value");
        hex.Text = "#NOPE";
        Check(!apply.IsEnabled, "invalid hex disables Apply");
        hex.Text = "#FFFFFF";
        Check(apply.IsEnabled && brightness.Value == 1, "valid hex restores Apply and synchronizes brightness");
        wheel.RaiseEvent(new Avalonia.Input.KeyEventArgs { RoutedEvent = Avalonia.Input.InputElement.KeyDownEvent, Key = Avalonia.Input.Key.Up });
        Check(hex.Text != "#FFFFFF", "wheel keyboard input updates editable hex");
        picker.Close();
    }

    public static void Run(string temporary)
    {
        string root = Path.Combine(temporary, "theme-checks");
        Directory.CreateDirectory(Path.Combine(root,"tools"));
        Directory.CreateDirectory(Path.Combine(root,"runtime"));
        File.WriteAllText(Path.Combine(root,"tools","os_mods.py"), "");
        var project = new Project(root, Project.CurrentPlatform);
        string ini = project.ConfigFile("launcher.ini");
        File.WriteAllText(ini,"[other]\nkeep = yes\n[theme]\ntheme_accent = invalid\n");
        var window = new MainWindow([],root,enableBackgroundActions:false);
        var surface = window.Surface;
        Check(surface.GetSetting("theme_accent")=="#FF3B30", "invalid saved color falls back to default");
        surface.ChangeSetting("theme_accent","#12abef");
        surface.ChangeSetting("theme_background","#112233");
        surface.ChangeSetting("theme_text","#EEEEDD");
        Check(surface.GetSetting("theme_accent")=="#12ABEF", "custom hex colors are normalized");
        Check(LogoPixel(surface,170,100)==new SkiaSharp.SKColor(0x12,0xAB,0xEF), "logo cross follows accent color");
        Check(LogoPixel(surface,300,100)==new SkiaSharp.SKColor(0xEE,0xEE,0xDD), "logo mark follows text color");
        Check(LogoPixel(surface,0,0).Alpha==0, "themed logo preserves transparency");
        Check(File.ReadAllText(ini).Contains("keep = yes"), "theme saves preserve unrelated preferences");
        string saved = File.ReadAllText(ini);
        surface.ChangeSetting("theme_accent","red;alert(1)");
        Check(File.ReadAllText(ini)==saved && surface.GetSetting("theme_accent")=="#12ABEF", "invalid color cannot overwrite saved theme");
        var reopened = new MainWindow([],root,enableBackgroundActions:false);
        Check(reopened.Surface.GetSetting("theme_accent")=="#12ABEF" && reopened.Surface.GetSetting("theme_background")=="#112233", "theme persists after reopening");
        string script = (string)typeof(ClassicSurface).GetMethod("ThemeScript",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(reopened.Surface,null)!;
        Check(script.Contains("#12ABEF") && script.Contains("--accent"), "marketplace receives custom palette");
        typeof(ClassicSurface).GetMethod("ResetTheme",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(reopened.Surface,null);
        Check(reopened.Surface.GetSetting("theme_accent")=="#FF3B30" && reopened.Surface.GetSetting("theme_text")=="#F4F4F2", "reset restores original colors");
        Check(surface.GetSetting("theme_accent")=="#12ABEF", "theme changes do not leak between windows");
        Check(LogoPixel(reopened.Surface,170,100)==new SkiaSharp.SKColor(255,59,48), "logo reset restores default accent");
        Check(LogoPixel(surface,170,100)==new SkiaSharp.SKColor(0x12,0xAB,0xEF), "logo themes stay independent between windows");
        PickerChecks(window);
        EyedropperChecks.Run(window);
        reopened.Close(); window.Close();
    }
}
