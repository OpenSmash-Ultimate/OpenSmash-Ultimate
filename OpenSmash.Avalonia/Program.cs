using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Avalonia.Media;

namespace OpenSmash;

internal static class Program
{
    internal static string[] Arguments = [];
    [STAThread]
    public static void Main(string[] args)
    {
        if(args.Contains("--licenses")) { Core.LegalNotices.Open(); return; }
        if(args.Contains("--source-info")) { Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(Core.BootstrapSource.Bundled())); return; }
        // (no window) --online-pass --project <folder>: a fresh lobby ticket in <folder>/build/cache/online_pass.txt from
        // the session signed in here, for a game started without this launcher (tools/mac/launch.sh, the web launcher)
        if(args.Contains("--online-pass"))
        {
            int at=Array.IndexOf(args,"--project");
            string root=Path.GetFullPath(at>=0&&at+1<args.Length?args[at+1]:Environment.CurrentDirectory);
            Environment.ExitCode=Core.OnlinePass.WriteForStartAsync(root,Console.WriteLine).GetAwaiter().GetResult();
            return;
        }
        Arguments = args;
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace()
            .StartWithClassicDesktopLifetime(args);
    }
}

public sealed class App : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        var theme = new FluentTheme();
        theme.Palettes[ThemeVariant.Dark] = new ColorPaletteResources { Accent = Color.Parse("#FF3B30") };
        Styles.Add(theme);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow(Program.Arguments);
        base.OnFrameworkInitializationCompleted();
    }
}
