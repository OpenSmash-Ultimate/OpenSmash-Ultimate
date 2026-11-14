// Native web-engine smoke test. No project/game is opened and no mod is installed.
// A restrictive CSP keeps the original page offline during this test.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using OpenSmash.Visuals;
using System.Text.Json;

static class Program
{
    [STAThread]
    static int Main(string[] args) => AppBuilder.Configure<BrowserTestApp>().UsePlatformDetect().StartWithClassicDesktopLifetime(args);
}

public sealed class BrowserTestApp : Application
{
    bool _done;
    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime lifetime)
        {
            var view = new NativeWebView { Background = Avalonia.Media.Brushes.Black };
            var window = new Window { Title = "OpenSmash browser verification", Width = 900, Height = 600, Content = view };
            lifetime.MainWindow = window;
            void Finish(int exit, string message)
            {
                if (_done) return;
                _done = true; Console.WriteLine(message); lifetime.Shutdown(exit);
            }
            view.WebMessageReceived += async (_, e) =>
            {
                try
                {
                    using var doc = JsonDocument.Parse(e.Body ?? "{}");
                    if (doc.RootElement.GetProperty("type").GetString() != "ready" || _done) return;
                    await view.InvokeScript("window.__osm.setInstalled(['123']); window.__osm.setInventory({plugins:['libparam_config'], mods:['fixture'], loaders:{skyline:true,arcropolis:false}});");
                    string? actual = await view.InvokeScript("JSON.stringify({transport:bridge.available,installed:state.installed.has('123')})");
                    using var value = JsonDocument.Parse(actual ?? "null");
                    var root = value.RootElement;
                    if (root.ValueKind == JsonValueKind.String)
                    {
                        using var nested = JsonDocument.Parse(root.GetString()!); root = nested.RootElement.Clone();
                    }
                    if (!root.GetProperty("transport").GetBoolean() || !root.GetProperty("installed").GetBoolean()) throw new Exception("Bridge state did not round trip.");
                    Finish(0, "PASS native browser loads the original HTML, receives ready, and round-trips installed state.");
                }
                catch (Exception error) { Finish(1, "FAIL native browser: " + error); }
            };
            view.NavigationCompleted += (_, e) => { if (!e.IsSuccess) Finish(1, "FAIL native browser navigation."); };
            window.Opened += async (_, _) =>
            {
                string html = ClassicSurface.MarketplaceHtml().Replace("<head>", "<head><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:\">");
                view.NavigateToString(html);
                await Task.Delay(TimeSpan.FromSeconds(30));
                if (!_done) Finish(1, "FAIL native browser did not send ready within 30 seconds.");
            };
        }
        base.OnFrameworkInitializationCompleted();
    }
}
