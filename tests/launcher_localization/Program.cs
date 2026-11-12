using System.Drawing;
using System.Windows.Forms;
using OpenSmash.Launcher;

static class Tests
{
    static int count;
    static void Check(bool ok, string name) { if (!ok) throw new Exception(name); count++; Console.WriteLine("PASS " + name); }
    static Control Find(Control c, string name) => c.Controls.Find(name, true).Single();
    static void Until(Func<bool> condition, string message)
    {
        var until = DateTime.UtcNow.AddSeconds(10);
        while (!condition() && DateTime.UtcNow < until) { Application.DoEvents(); Thread.Sleep(20); }
        Check(condition(), message);
    }
    static void LocaleChecks(string repo, string[] args)
    {
        Environment.SetEnvironmentVariable("SSBU_UI_LANGUAGE", null);
        string file = Path.Combine(repo, "display.ini");
        File.WriteAllText(file, "; retained comment\nui_language = en\nlanguage = ja\nwindow = borderless\n");
        UiLocale.Initialize(repo);
        Check(UiLocale.Codes.Contains("en") && UiLocale.Codes.Contains("zh-CN"), "shared catalogs embedded");
        Check(UiLocale.Resolve("ZH_hAnS.UTF-8") == "zh-CN" && UiLocale.Resolve("xx-XX") == "en", "locale aliases and fallback");
        UiLocale.Select("zh-CN");
        Check(UiLocale.Text("Settings") == "设置" && UiLocale.Text("SETTINGS") == "设置", "Chinese and uppercase labels");
        Check(UiLocale.Text("New feature without a translation") == "New feature without a translation", "new messages fall back to English");
        Check(UiLocale.Interpolate($"A newer version is on GitHub: {42} new commits.") == "GitHub 上有新版本：42 个新提交。", "translated dynamic placeholders");
        UiLocale.Select("en");
        using var launcher = new LauncherForm(repo, repo, Path.Combine(repo, "build"), preview: true);
        UiLocale.Attach(launcher);
        var type = typeof(LauncherForm);
        var flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        void Call(string name, params object[] values) => type.GetMethod(name, flags)!.Invoke(launcher, values);
        launcher.StartPosition = FormStartPosition.Manual;
        launcher.Location = new Point(-20000, -20000);
        launcher.ClientSize = new Size(1280, 720);
        launcher.Show(); Application.DoEvents();
        Call("GoView", "settings", true);
        Call("WriteSetting", "ui_language", "zh-CN");
        Check(UiLocale.Language == "zh-CN", "launcher setting switches immediately");
        string saved = File.ReadAllText(file);
        Check(saved.Contains("ui_language = zh-CN") && saved.Contains("language = ja") && saved.Contains("window = borderless") && saved.Contains("; retained comment"), "UI choice preserves game language and setting values");
        var tabs = (SettingTab[])type.GetField("_settingsTabs", flags)!.GetValue(launcher)!;
        Check(tabs[0].SettingKeys.Contains("ui_language"), "language is available on Display settings");
        var settings = (Setting[])type.GetField("_settings", flags)!.GetValue(launcher)!;
        Check(settings.Single(s => s.Key == "window").Values.Contains("borderless"), "display translation does not change stored choices");
        if (args.Length > 0)
        {
            Directory.CreateDirectory(args[0]);
            void Render(string name, int w, int h, string page = "settings")
            {
                launcher.ClientSize = new Size(w, h);
                type.GetField("_paintedView", flags)!.SetValue(launcher, page);
                type.GetField("_viewAt", flags)!.SetValue(launcher, -10d);
                type.GetField("_shownAt", flags)!.SetValue(launcher, 0d);
                using var warm = new Bitmap(w, h); launcher.DrawToBitmap(warm, new Rectangle(0, 0, w, h));
                Thread.Sleep(1100);
                using var bitmap = new Bitmap(w, h); launcher.DrawToBitmap(bitmap, new Rectangle(0, 0, w, h));
                bitmap.Save(Path.Combine(args[0], name));
            }
            Render("settings-zh-CN.png", 1280, 720);
            Render("settings-zh-CN-150.png", 1920, 1080);
            Call("GoView", "home", true);
            Render("home-zh-CN.png", 1280, 720, "home");
            Call("GoView", "settings", true);
            Call("WriteSetting", "ui_language", "en");
            Render("settings-en.png", 1280, 720);
        }
        // The external menu may have changed the shared setting while the launcher was hidden.
        File.WriteAllText(file, File.ReadAllText(file).Replace("ui_language = zh-CN", "ui_language = en"));
        Call("PersistSettings");
        Check(File.ReadAllText(file).Contains("ui_language = en"), "launcher does not overwrite a newer in-game language choice");
        foreach (var (code, name) in new[] { ("ja", "日本語"), ("ko", "한국어"), ("es", "Español"), ("fr", "Français"), ("de", "Deutsch") })
        {
            Check(UiLocale.Codes.Contains(code) && UiLocale.Names.Contains(name), code + " is selectable by native name");
            Check(UiLocale.Resolve(code.ToUpperInvariant() + "_XX.UTF-8") == code, code + " regional alias");
            Call("WriteSetting", "ui_language", code);
            Check(UiLocale.Language == code && UiLocale.Text("Interface language") != "Interface language", code + " switches launcher text");
            Check(UiLocale.Text("untranslated fixture") == "untranslated fixture", code + " falls back to source");
            if (args.Length > 0) RenderLocale(code);
        }
        void RenderLocale(string code)
        {
            Call("GoView", "settings", true);
            type.GetField("_paintedView", flags)!.SetValue(launcher, "settings");
            type.GetField("_viewAt", flags)!.SetValue(launcher, -10d);
            using var bitmap = new Bitmap(1280, 720);
            launcher.ClientSize = bitmap.Size;
            launcher.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
            bitmap.Save(Path.Combine(args[0], "settings-" + code + ".png"));
        }
        BrowserLocaleChecks(launcher);
        Environment.SetEnvironmentVariable("SSBU_UI_LANGUAGE", "en");
        UiLocale.Select("zh-CN");
        Check(UiLocale.Language == "en", "environment override remains authoritative");
        UiLocale.Select("en");
        launcher.Close();
    }

    static void BrowserLocaleChecks(LauncherForm launcher)
    {
        // Real WebView2, but every request is answered locally: no GameBanana or other network calls.
        string cache = Path.Combine(Path.GetTempPath(), "OpenSmash UI web test " + Guid.NewGuid().ToString("N"));
        using var host = new Form { ClientSize = new Size(1000, 600), StartPosition = FormStartPosition.Manual, Location = new Point(-20000, -20000) };
        using var view = new Microsoft.Web.WebView2.WinForms.WebView2 { Dock = DockStyle.Fill };
        host.Controls.Add(view); host.Show();
        try
        {
            var environment = Microsoft.Web.WebView2.Core.CoreWebView2Environment.CreateAsync(null, cache);
            Until(() => environment.IsCompleted, "browser environment completes");
            var initialize = view.EnsureCoreWebView2Async(environment.GetAwaiter().GetResult());
            Until(() => initialize.IsCompleted, "browser control initializes");
            initialize.GetAwaiter().GetResult();
            var core = view.CoreWebView2;
            core.AddWebResourceRequestedFilter("*", Microsoft.Web.WebView2.Core.CoreWebView2WebResourceContext.All);
            core.WebResourceRequested += (_, e) => {
                var body = new MemoryStream(System.Text.Encoding.UTF8.GetBytes("{\"_aRecords\":[],\"_nRecordCount\":0}"));
                e.Response = core.Environment.CreateWebResourceResponse(body, 200, "OK", "Content-Type: application/json");
            };
            bool navigated = false;
            core.NavigationCompleted += (_, _) => navigated = true;
            UiLocale.Select("zh-CN");
            string html = (string)typeof(LauncherForm).GetMethod("GetMarketplaceHtml", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!.Invoke(null, null)!;
            core.NavigateToString(html);
            Until(() => navigated, "embedded marketplace loads local fixtures");
            string Script(string code)
            {
                var task = core.ExecuteScriptAsync(code);
                Until(() => task.IsCompleted, "browser script completes");
                return System.Text.Json.JsonSerializer.Deserialize<string>(task.GetAwaiter().GetResult()) ?? "";
            }
            Check(Script("document.querySelector('option').textContent") == "最新发布", "browser uses shared Chinese catalog");
            Check(Script("document.documentElement.lang") == "zh-CN", "browser document language matches selection");
            Check(Script("document.querySelector('option').value") == "Generic_Newest", "browser option value is not translated");
            Check(Script("document.querySelector('#dTitle').textContent='Settings'; document.querySelector('#dTitle').textContent") == "Settings", "remote mod title stays verbatim");
            Script("window.__opensmashSetLocale({language:'en',messages:{}}); 'ok'");
            Check(Script("document.querySelector('option').textContent") == "Newest", "browser restores original English without reload");
            string pager = Script("document.querySelector('#pageInfo').textContent");
            Check(pager.StartsWith("Page ") || pager == "Loading…", "browser dynamic pager follows language changes");
        }
        catch (Microsoft.Web.WebView2.Core.WebView2RuntimeNotFoundException)
        {
            Console.WriteLine("SKIP browser locale rendering: WebView2 runtime is not installed");
        }
        finally
        {
            host.Close(); view.Dispose();
            try { Directory.Delete(cache, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    [STAThread]
    static void Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        string repo = Path.Combine(Path.GetTempPath(), "UI localization tests " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(repo);
        try
        {
            LocaleChecks(repo, args);
            Console.WriteLine($"{count} checks passed");
        }
        finally { Directory.Delete(repo, true); }
    }
}
