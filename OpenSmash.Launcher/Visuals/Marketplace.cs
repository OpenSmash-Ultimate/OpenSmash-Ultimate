using System.Drawing;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    NativeWebView? _marketplace;
    bool _marketplaceReady;
    public void MarketplaceLoaded()=>_marketplaceReady=true;
    string _marketplaceError = "";
    public event Action<string>? MarketplaceMessage;
    public string[] InstalledModIds => _mods.Where(m => !string.IsNullOrEmpty(m.GamebananaId)).Select(m => m.GamebananaId).Distinct().ToArray();
    public string[] InstalledModFolders => _mods.Select(m => m.Name).ToArray();

    // The shipped HTML/CSS is the original page. Only its transport changes on WebKit.
    public static string MarketplaceHtml()
    {
        using var stream = typeof(ClassicSurface).Assembly.GetManifestResourceStream("MarketplaceHtml.html")!;
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd().Replace("const wv = window.chrome && window.chrome.webview;",
            "const wv = { postMessage: value => invokeCSharpAction(JSON.stringify(value)) };")
            .Replace("/*@osm-fonts*/", MarketplaceFonts.Value);
    }

    // The page is loaded from a string, so it cannot reach avares:// assets: the launcher's own fonts go in as data URIs.
    // Cascadia Mono stays out to keep the page under WebView2's 2 MB NavigateToString limit; the CSS falls back to Consolas.
    static readonly Lazy<string> MarketplaceFonts = new(() =>
    {
        var css = new System.Text.StringBuilder();
        foreach (var (family, file, weight) in new[] { ("Anton", "Anton-Regular.ttf", 400), ("Inter", "Inter-Regular.ttf", 400), ("Inter", "Inter-Bold.ttf", 700) })
        {
            try
            {
                using var source = Avalonia.Platform.AssetLoader.Open(new Uri("avares://OpenSmash/Assets/" + file));
                using var bytes = new MemoryStream();
                source.CopyTo(bytes);
                css.Append("@font-face{font-family:\"").Append(family).Append("\";font-weight:").Append(weight)
                   .Append(";src:url(data:font/ttf;base64,").Append(Convert.ToBase64String(bytes.ToArray())).Append(") format(\"truetype\");}");
            }
            catch (Exception error) { System.Diagnostics.Trace.WriteLine(error); }   // the CSS's system fallbacks take over
        }
        return css.ToString();
    });

    public void EnsureMarketplace()
    {
        if (_marketplace != null || _marketplaceError.Length > 0) return;
        try
        {
            Avalonia.Platform.WebViewAdapterType[] candidates = OperatingSystem.IsWindows()
                ? [Avalonia.Platform.WebViewAdapterType.WebView2]
                : OperatingSystem.IsMacOS() ? [Avalonia.Platform.WebViewAdapterType.WkWebView]
                : [Avalonia.Platform.WebViewAdapterType.WpeWebKit, Avalonia.Platform.WebViewAdapterType.WebKitGtk];
            var available = candidates.Select(Avalonia.Platform.WebViewAdapterInfo.GetAdapterInfo).ToArray();
            if (!available.Any(info => info.IsInstalled))
                throw new InvalidOperationException(string.Join(" ", available.Select(info => info.UnavailableReason)));
            var view = new NativeWebView
            {
                Background = Avalonia.Media.Brush.Parse("#08090b"),
                AdapterPreference = available.Where(info => info.IsInstalled).Select(info => info.Type).ToArray()
            };
            view.NavigationStarted += (_, e) =>
            {
                if (e.Request is not { } uri || uri.Scheme is "about" or "data") return;
                e.Cancel = true;
                MarketplaceMessage?.Invoke(System.Text.Json.JsonSerializer.Serialize(new { type = "open_external", url = uri.AbsoluteUri }));
            };
            view.NewWindowRequested += (_, e) =>
            {
                e.Handled = true;
                MarketplaceMessage?.Invoke(System.Text.Json.JsonSerializer.Serialize(new { type = "open_external", url = e.Request?.AbsoluteUri }));
            };
            view.WebMessageReceived += (_, e) => { if(e.Body is {} body) MarketplaceMessage?.Invoke(body); };
            view.NavigationCompleted += (_, e) =>
            {
                if (!e.IsSuccess) MarketplaceFailed("The system web view could not load the mod browser.");
            };
            view.NavigateToString(MarketplaceHtml());
            _marketplace = view;
            LogicalChildren.Add(view);
            VisualChildren.Add(view);
            InvalidateMeasure();
            _ = CheckMarketplaceStartup();
        }
        catch (Exception error) { MarketplaceFailed(error.Message); }
    }

    async Task CheckMarketplaceStartup()
    {
        await Task.Delay(TimeSpan.FromSeconds(15));
        if(!_marketplaceReady) MarketplaceFailed("The system web view did not start.");
    }

    void MarketplaceFailed(string message)
    {
        _marketplaceError = "Mod browser unavailable: " + message + "\n" +
            (OperatingSystem.IsLinux() ? "Install GTK 3, WebKitGTK 4.1 and libsoup 3, then restart OpenSmash." :
             OperatingSystem.IsWindows() ? "Install the Microsoft Edge WebView2 Runtime, then restart OpenSmash." : "Restart OpenSmash and check your system WebKit installation.");
        UpdateMarketplace();
        InvalidateVisual();
    }

    public async Task MarketplaceScript(string script)
    {
        if (!Dispatcher.UIThread.CheckAccess()) { await Dispatcher.UIThread.InvokeAsync(() => MarketplaceScript(script)); return; }
        if (_marketplace != null) try { await _marketplace.InvokeScript(script); } catch (Exception error) { System.Diagnostics.Trace.WriteLine(error); }
    }

    void UpdateMarketplace()
    {
        if (_marketplace != null) _marketplace.IsVisible = !IsInstaller && _view == "mods_browser" && !HasPrompt && _marketplaceError.Length == 0;
    }

    void ArrangeMarketplace(Avalonia.Size size)
    {
        double s = size.Height / 720;
        // Original PaintModsBrowser rounds in physical pixels before assigning bounds.
        double scale = TopLevel.GetTopLevel(this)?.RenderScaling ?? 1;
        double y = Math.Round(176 * s * scale) / scale;
        _marketplace?.Arrange(new Rect(0, y, Math.Max(1, size.Width), Math.Max(1, size.Height - y - Math.Round(StatusH * s * scale) / scale - 1 / scale)));
    }

    void PaintMarketplace(Graphics g, int W, int H)
    {
        float s = _s, x = Gutter * s;
        PaintSectionHead(g, "02 / MODS", "BROWSE MODS");
        var state = PushFade(g, Up((float)(Now - _viewAt), .4f), 16 * s);
        ArtworkTheme.Label(g, "POWERED BY GAMEBANANA", FM(12 * s), A(ArtworkTheme.Dim), 0, SectionTitleTop * s + 14 * s, 20 * s, 12 * s * .16f, StringAlignment.Far, W - x);
        PopFade(g, state);
        if (_marketplaceError.Length > 0)
            ArtworkTheme.DrawText(g, _marketplaceError, FB(15 * s), ArtworkTheme.Bad, x, H / 2 - 40 * s, W - 2 * x, 90 * s, StringAlignment.Center, StringAlignment.Center);
    }
}
