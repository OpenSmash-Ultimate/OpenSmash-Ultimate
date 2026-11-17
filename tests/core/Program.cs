// SPDX-License-Identifier: GPL-3.0-or-later
// Checks of OpenSmash.Core (no network, no game): dotnet run --project tests/core. Exit code 0 when all pass.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using OpenSmash.Core;

static class Program
{
    static int failed, passed;

    static void Check(bool ok, string what)
    {
        if (ok) passed++; else { failed++; Console.WriteLine("FAIL " + what); }
    }

    // ---- the server region (ServerRegion.cs): the table, the choice, Auto's pick with made-up answer times (no network)
    static async Task Regions(string dir)
    {
        string was = Environment.GetEnvironmentVariable("SSBU_OFFICIAL_LOBBY");
        Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", null);
        var asker = ServerRegion.Asker;
        try
        {
            ServerRegion eu = ServerRegion.ById("eu"), na = ServerRegion.ById("na-east");
            const string naLobby = "na.lobby.opensmashultimate.com:39421";
            Check(ServerRegion.All.Length == 2 && ServerRegion.All[0] == eu && eu.Lobby == OfficialLobby.Default && eu.Label == "EU", "the first region: EU, the official lobby's default");
            Check(na != null && na.Lobby == naLobby && na.Label == "NA East" && na.Host == "na.lobby.opensmashultimate.com" && na.Port == 39421, "NA East and its lobby");
            Check(ServerRegion.All.All(r => OfficialLobby.Valid(r.Lobby) && !System.Net.IPAddress.TryParse(r.Host, out _) && r.Host.Any(char.IsLetter)), "every region's lobby is a name, never an address in numbers");
            Check(ServerRegion.ById("NA-East") == na && ServerRegion.ById(" eu ") == eu && ServerRegion.ById("auto") == null && ServerRegion.ById("na") == null && ServerRegion.ById(null) == null, "a region by its id");
            Check(ServerRegion.OfLobby(naLobby) == na && ServerRegion.OfLobby("NA.lobby.opensmashultimate.com") == na && ServerRegion.OfLobby(OfficialLobby.Host) == eu && ServerRegion.OfLobby("na.lobby.opensmashultimate.com:1") == null
                  && ServerRegion.OfLobby("") == null && ServerRegion.OfLobby("na.lobby.opensmashultimate.com.evil.example:39421") == null, "a region by its lobby");
            // the median of a few answers, and the pick
            Check(ServerRegion.Median(new[] { 30, 10, 20 }) == 20 && ServerRegion.Median(new[] { 40, 10 }) == 25 && ServerRegion.Median(new[] { -1, 12, -1 }) == 12
                  && ServerRegion.Median(new int[0]) == -1 && ServerRegion.Median(new[] { -1, -1 }) == -1, "the median of the answers that came");
            Check(ServerRegion.Pick(new[] { 20, 90 }) == eu && ServerRegion.Pick(new[] { 90, 20 }) == na, "Auto: the lowest time");
            Check(ServerRegion.Pick(new[] { -1, 140 }) == na && ServerRegion.Pick(new[] { 35, -1 }) == eu, "Auto: the only one that answers");
            Check(ServerRegion.Pick(new[] { -1, -1 }) == null && ServerRegion.Pick(new int[0]) == null, "Auto: none answers, no pick");
            Check(ServerRegion.Pick(new[] { 40, 40 }) == eu, "Auto: the same time, the first region");
            Check(ServerRegion.Pick(new[] { 52, 60 }, na) == na && ServerRegion.Pick(new[] { 60, 52 }, eu) == eu, "Auto: the last pick stays when the other is only a little sooner");
            Check(ServerRegion.Pick(new[] { 20, 90 }, na) == eu && ServerRegion.Pick(new[] { 50, 60 }, na) == eu, "Auto: the last pick gives way to one clearly sooner");
            Check(ServerRegion.Pick(new[] { 20, -1 }, na) == eu, "Auto: a last pick that does not answer gives way");
            Check(ServerRegion.ParseKept("na-east 31 1700000000") == (na, 31) && ServerRegion.ParseKept("eu") == (eu, -1) && ServerRegion.ParseKept("mars 5 1").Region == null
                  && ServerRegion.ParseKept("").Region == null && ServerRegion.ParseKept("eu -4 1") == (eu, -1) && ServerRegion.ParseKept(null).Region == null, "Auto's kept pick, read");

            // an install: no file is Auto, and Auto with nothing measured is the first region
            string root = Path.Combine(dir, "regions");
            Directory.CreateDirectory(Path.Combine(root, "config"));
            string setting = Path.Combine(root, "config", "server_region.txt"), kept = Path.Combine(root, "config", "server_region_auto.txt");
            Check(ServerRegion.Setting(root) == "auto" && ServerRegion.Setting(null) == "auto" && ServerRegion.Current(root) == eu && OfficialLobby.Current(root) == OfficialLobby.Default, "no file: Auto, and Europe until something is measured");
            Check(ServerRegion.Shown(root) == "Auto" && ServerRegion.AutoPick(root).Region == null, "Auto, not measured: shown as Auto");
            // Auto measured with made-up times: North America sooner
            int asked = 0;
            ServerRegion.Asker = (r, n) => { System.Threading.Interlocked.Increment(ref asked); return r == na ? new[] { 21, 25, 23 } : new[] { 95, 90, 99 }; };
            var got = await ServerRegion.Measure(root);
            Check(got == (na, 23) && asked == 2, "Auto measured: NA East, the median of its answers");
            Check(File.ReadAllText(kept).StartsWith("na-east 23 ") && ServerRegion.Kept(root) == (na, 23), "Auto's pick kept for the game (config/server_region_auto.txt)");
            Check(OfficialLobby.Current(root) == naLobby && ServerRegion.Current(root) == na && ServerRegion.Shown(root) == "Auto (NA East, 23 ms)", "Auto: the lobby is the pick's, shown with its ping");
            Check(OfficialLobby.Page("/status.json", root) == "http://" + naLobby + "/status.json", "the players online and the leaderboard are that region's");
            Check(OfficialLobby.All(root).SequenceEqual(new[] { naLobby, OfficialLobby.Default }), "every official lobby, the one played on first");
            // a little sooner in Europe now: it stays; clearly sooner: it moves
            ServerRegion.Asker = (r, n) => r == na ? new[] { 60 } : new[] { 55 };
            Check(await ServerRegion.Measure(root) == (na, 60), "measured again, Europe a little sooner: the pick stays");
            ServerRegion.Asker = (r, n) => r == na ? new[] { 120, 130 } : new[] { 30, 34 };
            Check(await ServerRegion.Measure(root) == (eu, 32) && OfficialLobby.Current(root) == OfficialLobby.Default, "measured again, Europe clearly sooner: Europe");
            // no lobby answers: the pick kept before stays, nothing is written over it
            ServerRegion.Asker = (r, n) => new int[0];
            Check(await ServerRegion.Measure(root) == ((ServerRegion)null, -1) && ServerRegion.Kept(root) == (eu, 32), "no lobby answers: the pick kept before stays");
            ServerRegion.Asker = (r, n) => r == eu ? throw new InvalidOperationException("no network") : new[] { 140 };
            Check(await ServerRegion.Measure(root) == (na, 140), "a region that cannot be asked counts as no answer");
            // a lobby that never answers is not waited for past the limit
            ServerRegion.Asker = (r, n) => { if (r == eu) System.Threading.Thread.Sleep(3000); return new[] { 50 }; };
            var clock = System.Diagnostics.Stopwatch.StartNew();
            int[] ms = await ServerRegion.MeasureAll(200);
            Check(ms.SequenceEqual(new[] { -1, 50 }) && clock.ElapsedMilliseconds < 2000, "a slow lobby is not waited for: " + clock.ElapsedMilliseconds + " ms");
            // nothing kept and no install: this process remembers what it measured
            string none = Path.Combine(dir, "no-install");
            ServerRegion.Asker = (r, n) => r == na ? new[] { 20 } : new[] { 90 };
            Check(await ServerRegion.Measure(none) == (na, 20) && !Directory.Exists(none) && OfficialLobby.Current(none) == naLobby, "no install folder: the pick is remembered, no folder made");

            // the region chosen: that one, whatever Auto measured
            ServerRegion.SetSetting(root, "eu");
            Check(File.ReadAllText(setting).Trim() == "eu" && ServerRegion.Setting(root) == "eu" && OfficialLobby.Current(root) == OfficialLobby.Default && ServerRegion.Shown(root) == "EU", "EU chosen");
            File.WriteAllText(setting, "# the server region\nNA-East   # mine\n");
            Check(ServerRegion.Setting(root) == "na-east" && OfficialLobby.Current(root) == naLobby && ServerRegion.Shown(root) == "NA East", "NA East chosen (as the game reads the file)");
            ServerRegion.SetSetting(root, "auto");
            Check(File.ReadAllText(setting).Replace("\r", "") == "# the server region\nauto\n" && ServerRegion.Setting(root) == "auto", "back to Auto: the file's comment kept");
            ServerRegion.SetSetting(root, "moon");
            Check(ServerRegion.Setting(root) == "auto", "a region that is none is Auto");
            File.WriteAllText(setting, "moon\n");
            Check(ServerRegion.Setting(root) == "auto" && ServerRegion.Current(root) == na, "a file that names no region: Auto (the kept pick)");

            // an override is that server: no regions, nothing of Auto
            File.WriteAllText(setting, "na-east\n");
            File.WriteAllText(Path.Combine(root, "config", "official_lobby.txt"), "lobby.example:1\n");
            Check(OfficialLobby.Override(root) == "lobby.example:1" && OfficialLobby.Current(root) == "lobby.example:1" && ServerRegion.Current(root) == null && ServerRegion.Shown(root) == "lobby.example:1", "the release's lobby wins over the region chosen");
            Check(OfficialLobby.All(root).SequenceEqual(new[] { "lobby.example:1" }), "an override: that server alone");
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", naLobby);
            Check(OfficialLobby.Current(root) == naLobby && ServerRegion.Current(root) == null, "SSBU_OFFICIAL_LOBBY first, a region's lobby too: that server, no Auto");
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", null);
            File.Delete(Path.Combine(root, "config", "official_lobby.txt"));
            Check(OfficialLobby.Override(root) == null && OfficialLobby.Override() == null && OfficialLobby.Current(root) == naLobby, "no override: the regions decide");
            // any region's lobby in online_server.txt means the official lobby
            Check(OfficialLobby.Means(naLobby) && OfficialLobby.Means("na.lobby.opensmashultimate.com") && !OfficialLobby.Means("na.lobby.opensmashultimate.com.evil.example:39421"), "a region's lobby means the official lobby");
        }
        finally
        {
            ServerRegion.Asker = asker;
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", was);
        }
    }

    static async Task<int> Main()
    {
        // ---- versions
        Check(SemVer.Parse("0.4.0") > SemVer.Parse("0.4.0-rc.2"), "release newer than its rc");
        Check(SemVer.Parse("0.4.0-rc.10") > SemVer.Parse("0.4.0-rc.2"), "numeric pre-release parts");
        Check(SemVer.Parse("v0.4.0") > SemVer.Parse("0.3.0-alpha.3"), "0.4.0 newer than the alphas");
        Check(SemVer.Parse("0.4.1") > SemVer.Parse("0.4.0"), "patch");
        Check(SemVer.Parse("0.4.0.0").ToString() == "0.4.0", "assembly version form");
        Check(!SemVer.TryParse("main", out _), "a branch is no version");
        Check(Updater.Current.ToString() == "0.4.0-test", "Current from the informational version: " + Updater.Current);

        // ---- a release in a folder (OPENSMASH_RELEASE_DIR)
        string dir = Path.Combine(Path.GetTempPath(), "opensmash-core-test-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(dir);
        try
        {
            byte[] exe = Encoding.ASCII.GetBytes("not really a launcher");
            File.WriteAllBytes(Path.Combine(dir, "OpenSmash.exe"), exe);
            string commit = new string('a', 40);
            string manifest = "{\"schema\":1,\"version\":\"0.4.0\",\"commit\":\"" + commit + "\",\"channel\":\"stable\",\"lobby\":\"lobby.example:1\",\"min_client_level\":1," +
                              "\"launcher\":{\"" + Releases.Platform + "\":{\"file\":\"OpenSmash.exe\",\"sha256\":\"" + Releases.Sha256(exe) + "\"}," +
                              "\"evil\":{\"file\":\"../x.exe\",\"sha256\":\"00\"}}}";
            File.WriteAllText(Path.Combine(dir, Releases.ManifestName), manifest);
            Environment.SetEnvironmentVariable("OPENSMASH_RELEASE_DIR", dir);
            var r = await Releases.LatestAsync("stable");
            Check(r != null && r.Version.ToString() == "0.4.0" && r.Commit == commit && r.Lobby == "lobby.example:1" && r.MinClientLevel == 1, "manifest read");
            Check(r != null && !r.Launcher.ContainsKey("evil"), "a file with a path is not listed");
            Check(Updater.LauncherOutdated(r), "0.4.0 is newer than 0.4.0-test");
            byte[] got = await Releases.DownloadCheckedAsync(r.Launcher[Releases.Platform]);
            Check(got.SequenceEqual(exe), "checked download");
            r.Launcher[Releases.Platform].Sha256 = new string('0', 64);
            bool refused = false;
            try { await Releases.DownloadCheckedAsync(r.Launcher[Releases.Platform]); } catch (InvalidDataException) { refused = true; }
            Check(refused, "a wrong SHA-256 is refused");
            Environment.SetEnvironmentVariable("OPENSMASH_RELEASE_DIR", null);

            // ---- installed commit, channel, game outdated
            string root = Path.Combine(dir, "install");
            Directory.CreateDirectory(Path.Combine(root, "tools"));
            File.WriteAllText(Path.Combine(root, "tools", "source_version.txt"), commit + " 2026-11-16\n");
            Check(Updater.InstalledCommit(root) == commit, "installed commit");
            Check(!Updater.GameOutdated(r, root), "same commit: up to date");
            File.WriteAllText(Path.Combine(root, "tools", "source_version.txt"), new string('b', 40) + "\n");
            Check(Updater.GameOutdated(r, root), "another commit: outdated");
            Directory.CreateDirectory(Path.Combine(root, ".git"));
            Check(!Updater.GameOutdated(r, root), "a git checkout is never changed");
            Check(Updater.Channel(root) == "stable", "stable by default");
            Updater.SetChannel(root, "testing");
            Check(Updater.Channel(root) == "testing", "testing kept");
            Updater.WriteLobby(r, root);
            Check(File.ReadAllText(Path.Combine(root, "config", "official_lobby.txt")).Trim() == "lobby.example:1", "official lobby written");
            // ---- the official lobby: a DNS name, the release's or the variable's when there is one
            string was = Environment.GetEnvironmentVariable("SSBU_OFFICIAL_LOBBY");
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", null);
            Check(OfficialLobby.Default == "eu.lobby.opensmashultimate.com:39421" && OfficialLobby.Default == OfficialLobby.Host + ":" + OfficialLobby.Port, "official lobby: its name and port");
            Check(!System.Net.IPAddress.TryParse(OfficialLobby.Host, out _) && OfficialLobby.Host.Any(char.IsLetter), "official lobby: a name, never an address in numbers");
            Check(OfficialLobby.Current() == OfficialLobby.Default && OfficialLobby.Current(Path.Combine(dir, "none")) == OfficialLobby.Default, "no install, no file: the built-in name");
            Check(OfficialLobby.Current(root) == "lobby.example:1", "the release's lobby (config/official_lobby.txt)");
            Check(OfficialLobby.Page("/status.json", root) == "http://lobby.example:1/status.json" && OfficialLobby.Page("/leaderboard.json") == "http://eu.lobby.opensmashultimate.com:39421/leaderboard.json", "the lobby's read-only pages");
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", "other.example:39421");
            Check(OfficialLobby.Current(root) == "other.example:39421", "SSBU_OFFICIAL_LOBBY first");
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", "http://x/ y");
            Check(OfficialLobby.Current(root) == "lobby.example:1", "a variable that is not host:port is not used");
            Environment.SetEnvironmentVariable("SSBU_OFFICIAL_LOBBY", was);
            foreach (string bad in new[] { "", "host", "host:0", "host:70000", "host:39421/x", "user@host:1", "a b:1", ":39421", "host:39421:2x" })
                Check(!OfficialLobby.Valid(bad), "not a lobby: '" + bad + "'");
            foreach (string h in new[] { "auto", "na", "eu", "AUTO", OfficialLobby.Default, OfficialLobby.Host, "opensmash-lobby.duckdns.org:39421", "smash.recomps.wtf:39421", "smash.recomps.wtf" })
                Check(OfficialLobby.Means(h), "means the official lobby: " + h);
            foreach (string h in new[] { "", "off", "lan", "my.host:39420", "eu.lobby.opensmashultimate.com.evil.example:39421" })
                Check(!OfficialLobby.Means(h), "does not mean the official lobby: '" + h + "'");
            Check(OfficialLobby.Means("lobby.example:1", root) && !OfficialLobby.Means("lobby.example:1"), "the install's own official lobby means it");
            await Regions(dir);

            // ---- unpacking a source archive
            string unpack = Path.Combine(dir, "unpack");
            Updater.Unpack(Zip(("top/tools/a.txt", "a"), ("top/b.txt", "b")), unpack);
            Check(File.ReadAllText(Path.Combine(unpack, "tools", "a.txt")) == "a" && File.Exists(Path.Combine(unpack, "b.txt")), "archive unpacked without its top folder");
            bool unsafeRefused = false;
            try { Updater.Unpack(Zip(("top/../../evil.txt", "x")), unpack); } catch (InvalidDataException) { unsafeRefused = true; }
            Check(unsafeRefused && !File.Exists(Path.Combine(dir, "evil.txt")), "zip-slip refused");
        }
        finally { try { Directory.Delete(dir, true); } catch { } }

        // ---- sign-in
        string ch = Account.Challenge("dBjftJeZ4CVP-mJ92K9pX6ZBoE48s6RA5i7Bvy9Xs4A");
        Check(ch == "71DBvQQZsrGoFaRh-1d6l-Zc6xECrtY4zrnhY0seOVU" && ch.Length == 43, "PKCE S256: base64url of SHA-256, no padding (" + ch + ")");
        var cb = Account.Callback.Parse("GET /callback/abc?code=xyz HTTP/1.1", "abc");
        Check(cb.Code == "xyz" && cb.RefreshToken == null, "PKCE callback");
        cb = Account.Callback.Parse("GET /callback?state=abc&refresh_token=r%2B1 HTTP/1.1", "abc");
        Check(cb.RefreshToken == "r+1" && cb.Code == null, "sign-in page callback");
        Check(Account.Callback.Parse("GET /callback/zzz?code=xyz HTTP/1.1", "abc").Code == null, "wrong state ignored");
        Check(Account.Callback.Parse("GET /callback?state=abc&code=xyz HTTP/1.1", "abc").Code == null, "a code needs the path state");
        Check(Account.Callback.Parse("GET /favicon.ico HTTP/1.1", "abc").Code == null, "stray request ignored");
        Check(Account.Callback.Parse("GET /callback/abc?error=access_denied&error_description=The+user+said+no HTTP/1.1", "abc").Error == "The user said no", "error shown");
        var svc = AccountService.Load(null);
        Check(!svc.Configured || Environment.GetEnvironmentVariable("OPENSMASH_SUPABASE_URL") != null, "the repository's build has no service set");
        // the sign-in page: OpenSmash's accounts site by default, whose /launcher page answers /callback?state=..&refresh_token=..
        if (!svc.Configured && Environment.GetEnvironmentVariable("OPENSMASH_SIGNIN_PAGE") == null)      // (the repository build: a release may name its own pages)
        {
            Check(svc.SignInPage == "https://account.opensmashultimate.com/launcher" && AccountService.DefaultSignInPage == svc.SignInPage, "default sign-in page: the accounts site (" + svc.SignInPage + ")");
            Check(svc.AccountPage == "https://account.opensmashultimate.com/account", "default account page");
            string sroot = Path.Combine(Path.GetTempPath(), "opensmash-svc-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path.Combine(sroot, "config"));
            string sfile = Path.Combine(sroot, "config", "account_service.json");
            try
            {
                File.WriteAllText(sfile, "{\"sign_in_page\":\"direct\"}");
                Check(AccountService.Load(sroot).SignInPage == null, "\"direct\": no sign-in page, Discord straight from Supabase (PKCE)");
                File.WriteAllText(sfile, "{\"url\":\"https://example.supabase.co\",\"publishable_key\":\"k\"}");
                var own = AccountService.Load(sroot);
                Check(own.Configured && own.SignInPage == null && own.AccountPage == null, "a service of one's own has no accounts site unless it names one");
                File.WriteAllText(sfile, "{\"url\":\"https://example.supabase.co\",\"publishable_key\":\"k\",\"sign_in_page\":\"https://example.org/launcher\"}");
                Check(AccountService.Load(sroot).SignInPage == "https://example.org/launcher", "a service's own sign-in page");
                File.WriteAllText(sfile, "{\"sign_in_page\":\"http://example.org/launcher\"}");
                Check(AccountService.Load(sroot).SignInPage == AccountService.DefaultSignInPage, "a sign-in page must be https: an http one is not used");
                File.WriteAllText(sfile, "{\"sign_in_page\":\"http://127.0.0.1:3012/launcher\"}");
                Check(AccountService.Load(sroot).SignInPage == "http://127.0.0.1:3012/launcher", "a sign-in page on this PC (tests)");
            }
            finally { try { Directory.Delete(sroot, true); } catch { } }
        }

        // ---- online pass, reports
        Check(OnlinePass.Expiry("00000000-0000-4000-8000-000000000001|Alice|1800000000." + new string('f', 64)) == 1800000000, "ticket expiry");
        Check(OnlinePass.Expiry("u|n|1800000000|1700000000." + new string('f', 64)) == 1800000000, "ticket v2 expiry");
        Check(OnlinePass.Expiry("garbage") == 0, "not a ticket");
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string dirty = "at " + home + "\\x.txt by " + Environment.UserName + " from 93.184.216.34 room 10.13.0.5 mail a.b@example.com pass " +
                       "00000000-0000-4000-8000-000000000001|Alice|1800000000." + new string('f', 64) + " at C:\\Games\\OpenSmash\\build";
        string clean = CrashReport.Clean(dirty, "C:\\Games\\OpenSmash");
        Check(!clean.Contains(home) && !clean.Contains("93.184.216.34") && clean.Contains("10.13.0.5") && !clean.Contains("example.com") &&
              clean.Contains("<pass>") && clean.Contains("<install>\\build"), "report cleaned: " + clean);

        // ---- the session's performance summary
        string perf = "{\"schema\":1,\"clean_exit\":true,\"session_s\":300,\"frames\":18000,\"ft_p50\":16.62,\"ft_p99\":21.5,\"stutters\":3," +
                      "\"hist\":{\"<8\":0,\"<12\":1,\">=100\":2},\"rb_rollbacks\":120,\"path\":\"C:/Users/someone\",\"name\":\"someone\"}";
        var pp = PerfReport.Payload(perf, new Dictionary<string, object> { ["cpu"] = "Test CPU", ["ram_gb"] = 16.0 }, "[vulkan] device: Test GPU (driver 1)\nrenderer: vulkan\n", "0.4.0", "abc");
        Check(pp != null && (double)pp["frames"] == 18000 && (string)pp["cpu"] == "Test CPU" && (string)pp["gpu"] == "Test GPU" && (string)pp["renderer"] == "vulkan" &&
              (bool)pp["crashed"] == false && !pp.ContainsKey("path") && !pp.ContainsKey("name") && ((Dictionary<string, long>)pp["hist"])[">=100"] == 2, "performance summary payload");
        Check(!System.Text.Json.JsonSerializer.Serialize(pp).Contains("someone"), "performance summary: unknown fields dropped");
        Check(PerfReport.Payload(perf.Replace("\"session_s\":300", "\"session_s\":20"), null, null, "0.4.0", "abc") == null, "performance summary: short sessions are not sent");
        Check(PerfReport.Payload("not json", null, null, "0.4.0", "abc") == null, "performance summary: not a summary");
        var pc = PerfReport.Pc();
        Check(pc.ContainsKey("cpu_threads") && !System.Text.Json.JsonSerializer.Serialize(pc).Contains(Environment.UserName, StringComparison.OrdinalIgnoreCase), "this PC's kind has no user name");
        string troot = Path.Combine(Path.GetTempPath(), "os-core-perf-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(troot);
        Check(PerfReport.Enabled(troot), "performance summaries: on by default");
        PerfReport.SetEnabled(troot, false);
        Check(!PerfReport.Enabled(troot), "performance summaries: off in the settings");
        Directory.Delete(troot, true);

        Console.WriteLine($"{passed} passed, {failed} failed");
        return failed == 0 ? 0 : 1;
    }

    static byte[] Zip(params (string name, string text)[] files)
    {
        using var m = new MemoryStream();
        using (var z = new ZipArchive(m, ZipArchiveMode.Create, true))
            foreach (var (name, text) in files)
                using (var s = z.CreateEntry(name).Open()) s.Write(Encoding.UTF8.GetBytes(text));
        return m.ToArray();
    }
}
