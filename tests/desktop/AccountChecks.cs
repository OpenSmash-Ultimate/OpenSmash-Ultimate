using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using Avalonia.Headless;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Core;

// (v0.5.0) The account on Windows, Linux and macOS (MainWindow.Account.cs over OpenSmash.Core.Account), against
// tools/dev/fake_accounts.py on 127.0.0.1 (no real service, no real account): sign-in in the "browser" (the authorize
// URL fetched, its 302 followed back to the launcher's loopback address: PKCE), the session kept for this user only
// (0600 off Windows; CI's Linux and macOS legs check that), resumed by a new launcher, the lobby ticket written for the
// game and named to it by SSBU_ONLINE_TICKET_FILE, sign-out removing both, and the account card's buttons.
static class AccountChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }

    static string Repo()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "tools", "dev", "fake_accounts.py"))) return d.FullName;
        throw new Exception("tools/dev/fake_accounts.py not found above " + AppContext.BaseDirectory);
    }

    static int FreePort() { var l = new TcpListener(IPAddress.Loopback, 0); l.Start(); int p = ((IPEndPoint)l.LocalEndpoint).Port; l.Stop(); return p; }

    /// <summary>the player's browser: the authorize page asked, its redirect followed to the launcher</summary>
    static void Browser(string url) => _ = Task.Run(async () =>
    {
        using var http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = true }) { Timeout = TimeSpan.FromSeconds(20) };
        try { using var _r = await http.GetAsync(url); } catch (HttpRequestException) { } catch (TaskCanceledException) { }
    });

    static bool OwnerOnly(string file) =>
        OperatingSystem.IsWindows() || File.GetUnixFileMode(file) == (UnixFileMode.UserRead | UnixFileMode.UserWrite);

    /// <summary>a task on the UI thread, the dispatcher pumped until it ends (the window's code invalidates visuals)</summary>
    static T OnUi<T>(Func<Task<T>> work)
    {
        var task = Dispatcher.UIThread.InvokeAsync(work);
        var until = DateTime.UtcNow.AddSeconds(60);
        while (!task.IsCompleted && DateTime.UtcNow < until) { Dispatcher.UIThread.RunJobs(); Thread.Sleep(10); }
        if (!task.IsCompleted) throw new Exception("timed out on the UI thread");
        return task.GetAwaiter().GetResult();
    }

    public static void Run(string temporary, string fixture)
    {
        string repo = Repo(), root = Path.Combine(temporary, "account " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "runtime")); Directory.CreateDirectory(Path.Combine(root, "tools"));
        File.WriteAllText(Path.Combine(root, "tools", "os_mods.py"), "");
        string exe = new Project(root, Project.CurrentPlatform).GameExecutable;   // (a built game: the home page, not the installer)
        Directory.CreateDirectory(Path.GetDirectoryName(exe)!); File.WriteAllText(exe, "synthetic test fixture");
        string secret = Path.Combine(root, "ticket_secret.txt");
        File.WriteAllText(secret, "test-secret-not-a-real-one\n");
        int port = FreePort();
        string python = OperatingSystem.IsWindows() ? "python" : "python3";
        var info = new ProcessStartInfo(python) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        foreach (var a in new[] { "-I", Path.Combine(repo, "tools", "dev", "fake_accounts.py"), "--port", port.ToString(), "--secret-file", secret, "--name", "Tester" }) info.ArgumentList.Add(a);
        using var service = Process.Start(info) ?? throw new Exception("python did not start");
        string? url0 = Environment.GetEnvironmentVariable("OPENSMASH_SUPABASE_URL"), key0 = Environment.GetEnvironmentVariable("OPENSMASH_SUPABASE_KEY"),
                page0 = Environment.GetEnvironmentVariable("OPENSMASH_SIGNIN_PAGE");
        try
        {
            if (!(service.StandardOutput.ReadLine() ?? "").Contains("fake accounts service on")) throw new Exception("fake_accounts.py did not start: " + service.StandardError.ReadToEnd());
            _ = service.StandardOutput.ReadToEndAsync(); _ = service.StandardError.ReadToEndAsync();
            Environment.SetEnvironmentVariable("OPENSMASH_SUPABASE_URL", "http://127.0.0.1:" + port);
            Environment.SetEnvironmentVariable("OPENSMASH_SUPABASE_KEY", "test-key");
            Environment.SetEnvironmentVariable("OPENSMASH_SIGNIN_PAGE", null);

            var window = new MainWindow([], root, enableBackgroundActions: false);
            window.Show(); window.Width = 1440; window.Height = 810;
            var surface = window.Surface; surface.FrozenTime = 10;
            Check(surface.CurrentView == "home", "the account is checked on the home page");
            var c = surface.Community;
            window.AccountInit(root);
            var account = window.AccountForTests!;
            Check(c.AccountMode && !c.SignedIn && account.Service.SignInPage == null, "a service in the build: the account replaces the Discord sign-in (a test service: PKCE, no sign-in page)");
            account.OpenBrowser = Browser;
            c.Enabled = true;
            var hits = (List<(string Id, System.Drawing.RectangleF R)>)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_hits", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(surface)!;
            using (window.CaptureRenderedFrame()) { }
            Check(hits.Any(h => h.Id == "community:signin"), "signed out: the home page offers SIGN IN");

            OnUi(async () => { await window.AccountAction("community:signin", root); return 0; });
            Check(c.SignedIn && c.Error == "" && c.ShownName == "Tester#1234", "sign-in in the browser (PKCE through the loopback address): the account card shows name#tag (" + c.ShownName + ")");
            string session = Path.Combine(root, "config", "session.dat");
            Check(File.Exists(session) && OwnerOnly(session), "the session is kept for this user only (DPAPI on Windows, a 0600 file elsewhere)");
            var again = new Account(root);
            Check(Task.Run(() => again.ResumeAsync()).GetAwaiter().GetResult() && again.Me?.Display == "Tester#1234", "a new launcher resumes the session");

            c.Open = true; surface.InvalidateVisual();
            using (window.CaptureRenderedFrame()) { }
            Check(hits.Any(h => h.Id == "community:signout") && hits.Any(h => h.Id == "community:shown") && !hits.Any(h => h.Id == "community:manage"),
                  "the account card: leaderboard choice and sign out (no Manage account without an account page)");
            c.AccountPage = true; surface.InvalidateVisual();
            using (window.CaptureRenderedFrame()) { }
            Check(hits.Any(h => h.Id == "community:manage"), "the account card offers Manage account when the service has an account page");

            // Play: a fresh ticket in build/cache/online_pass.txt, named to the game
            var env = OnUi(() => window.AccountPassForGame(root));
            string pass = Path.Combine(root, "build", "cache", "online_pass.txt");
            Check(env != null && env["SSBU_ONLINE_TICKET_FILE"] == pass && File.Exists(pass) && OwnerOnly(pass), "Play: the lobby ticket written for the game (this user only), SSBU_ONLINE_TICKET_FILE names it");
            string ticket = File.ReadAllText(pass).Trim();
            long exp = OnlinePass.Expiry(ticket), now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            Check(ticket.StartsWith("00000000-0000-4000-8000-0000000000aa|Tester|") && exp > now && exp <= now + 700, "the ticket is the account's, with its expiry (renewed at half life while the game runs)");
            var project = new Project(root, Project.CurrentPlatform);
            var play = Commands.Play(project, true) with { Environment = env };
            Check(play.StartInfo().Environment["SSBU_ONLINE_TICKET_FILE"] == pass, "the game's command carries the ticket file");
            string log = Path.Combine(root, "ticket-game.log");
            Task.Run(() => GameLaunch.Monitor(new Command("dotnet", [fixture, "--ticket-fixture"], root) { Environment = env }, log, () => { })).GetAwaiter().GetResult();
            Check(File.ReadAllText(log).Contains("TICKET_FILE=" + pass), "the started game sees SSBU_ONLINE_TICKET_FILE");
            window.AccountGameEnded(root);

            OnUi(async () => { await window.AccountAction("community:signout", root); return 0; });
            Check(!c.SignedIn && !File.Exists(session) && !File.Exists(pass), "sign-out forgets the session and removes the game's ticket");
            Check(OnUi(() => window.AccountPassForGame(root)) == null, "signed out: Play names no ticket (the game stays off the official lobbies and says why)");
            window.Close();
        }
        finally
        {
            Environment.SetEnvironmentVariable("OPENSMASH_SUPABASE_URL", url0);
            Environment.SetEnvironmentVariable("OPENSMASH_SUPABASE_KEY", key0);
            Environment.SetEnvironmentVariable("OPENSMASH_SIGNIN_PAGE", page0);
            try { service.Kill(true); } catch (InvalidOperationException) { }
        }
    }

    // The server region row (v0.5.0, OpenSmash.Core.ServerRegion): on the Online tab, read from and written to
    // config/server_region.txt (the game's menu writes it too), greyed out for a lobby of one's own.
    public static void Region(string temporary)
    {
        string root = Path.Combine(temporary, "region " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "runtime")); Directory.CreateDirectory(Path.Combine(root, "tools")); Directory.CreateDirectory(Path.Combine(root, "config"));
        File.WriteAllText(Path.Combine(root, "tools", "os_mods.py"), "");
        File.WriteAllText(Path.Combine(root, "config", "version.ini"), "1\n");
        File.WriteAllText(Path.Combine(root, "config", "online_server.txt"), "auto\n");
        File.WriteAllText(Path.Combine(root, "config", "server_region.txt"), "# the server region\nna-east\n");
        var window = new MainWindow([], root, enableBackgroundActions: false);
        var s = window.Surface;
        var t = typeof(OpenSmash.Visuals.ClassicSurface);
        var values = (Dictionary<string, string>)t.GetField("_iniValues", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(s)!;
        t.GetMethod("LoadSettings", BindingFlags.Instance | BindingFlags.NonPublic)?.Invoke(s, null);
        Check(values.GetValueOrDefault("region") == "na-east", "Server region read from config/server_region.txt");
        var tabs = (Array)t.GetField("_settingsTabs", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(s)!;
        var online = tabs.Cast<object>().First(x => (string)x.GetType().GetField("Name")!.GetValue(x)! == "Online");
        Check(((string[])online.GetType().GetField("SettingKeys")!.GetValue(online)!).Last() == "region", "the Online tab has the Server region row");
        values["region"] = "eu";
        ((HashSet<string>)t.GetField("_changedSettings", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(s)!).Add("region");
        t.GetMethod("WriteSettingsFiles", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(s, null);
        var lines = File.ReadAllLines(Path.Combine(root, "config", "server_region.txt"));
        Check(lines.Length == 2 && lines[0].StartsWith("#") && lines[1] == "eu" && ServerRegion.Setting(root) == "eu", "a changed region is written for the game (its comments kept)");
        window.Close();
    }
}
