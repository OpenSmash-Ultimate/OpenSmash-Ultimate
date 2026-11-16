// SPDX-License-Identifier: GPL-3.0-or-later
// Checks of OpenSmash.Core (no network, no game): dotnet run --project tests/core. Exit code 0 when all pass.
using System;
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

    static async Task<int> Main()
    {
        // ---- versions
        Check(SemVer.Parse("0.6.0") > SemVer.Parse("0.6.0-rc.2"), "release newer than its rc");
        Check(SemVer.Parse("0.6.0-rc.10") > SemVer.Parse("0.6.0-rc.2"), "numeric pre-release parts");
        Check(SemVer.Parse("v0.6.0") > SemVer.Parse("0.3.0-alpha.3"), "0.6.0 newer than the alphas");
        Check(SemVer.Parse("0.6.1") > SemVer.Parse("0.6.0"), "patch");
        Check(SemVer.Parse("0.6.0.0").ToString() == "0.6.0", "assembly version form");
        Check(!SemVer.TryParse("main", out _), "a branch is no version");
        Check(Updater.Current.ToString() == "0.6.0-test", "Current from the informational version: " + Updater.Current);

        // ---- a release in a folder (OPENSMASH_RELEASE_DIR)
        string dir = Path.Combine(Path.GetTempPath(), "opensmash-core-test-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(dir);
        try
        {
            byte[] exe = Encoding.ASCII.GetBytes("not really a launcher");
            File.WriteAllBytes(Path.Combine(dir, "OpenSmash.exe"), exe);
            string commit = new string('a', 40);
            string manifest = "{\"schema\":1,\"version\":\"0.6.0\",\"commit\":\"" + commit + "\",\"channel\":\"stable\",\"lobby\":\"lobby.example:1\",\"min_client_level\":1," +
                              "\"launcher\":{\"" + Releases.Platform + "\":{\"file\":\"OpenSmash.exe\",\"sha256\":\"" + Releases.Sha256(exe) + "\"}," +
                              "\"evil\":{\"file\":\"../x.exe\",\"sha256\":\"00\"}}}";
            File.WriteAllText(Path.Combine(dir, Releases.ManifestName), manifest);
            Environment.SetEnvironmentVariable("OPENSMASH_RELEASE_DIR", dir);
            var r = await Releases.LatestAsync("stable");
            Check(r != null && r.Version.ToString() == "0.6.0" && r.Commit == commit && r.Lobby == "lobby.example:1" && r.MinClientLevel == 1, "manifest read");
            Check(r != null && !r.Launcher.ContainsKey("evil"), "a file with a path is not listed");
            Check(Updater.LauncherOutdated(r), "0.6.0 is newer than 0.6.0-test");
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
