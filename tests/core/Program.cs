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

    // ---- "Verify game files" (FileCheck.cs) on a made-up install: whole, then each kind of damage and its repair
    static void FileChecks()
    {
        string root = Path.Combine(Path.GetTempPath(), "os-core-check-" + Guid.NewGuid().ToString("N"));
        string outside = Path.Combine(Path.GetTempPath(), "os-core-game-" + Guid.NewGuid().ToString("N"));
        void Put(string rel, string text = "data") { string f = Path.Combine(root, rel); Directory.CreateDirectory(Path.GetDirectoryName(f)); File.WriteAllText(f, text); }
        try
        {
            Directory.CreateDirectory(outside);
            string xci = Path.Combine(outside, "game.xci");
            File.WriteAllText(xci, "the game's own file");
            Check(FileCheck.Check(root).Any(p => p.Repair == FileRepair.Setup), "file check: an empty folder is sent to the set-up");
            Put("dump/build_folders.txt", "game=" + outside + "\nkeys=k\n");
            foreach (string n in new[] { "main", "main.npdm", "rtld", "sdk", "subsdk0" }) Put("dump/exefs/" + n);
            Put("dump/nro/lua2cpp_mario.nro", "a fighter's module");
            Put("dump/fonts/a.bfttf");
            Put("dump/modules/romfs_patch.bin", "patch data");
            Put("dump/modules/romfs.layers", "layers");
            Put("dump/modules/arcropolis.reloc.bin");
            Put("dump/modules/romfs.cfg", "layered\n" + Path.Combine(root, "dump", "modules", "romfs.layers") + "\n" + xci + "\n" + Path.Combine(root, "dump", "modules", "romfs_patch.bin") + "\n");
            Put("dump/skyline/plugins.txt", "arcropolis\n");
            Put("dump/shadercache/complete.txt", "version 10\nspirv g1\n");
            Put("build/ssbu.exe", "MZ");
            Put("build/ssbu_glslang.dll", "MZ");
            var bad = FileCheck.Check(root);
            Check(bad.Count == 0, "file check: a whole install has no problem: " + FileCheck.Report(bad));
            Check(!FileCheck.HasList(root) && FileCheck.WriteList(root) == 11 && FileCheck.HasList(root), "file check: the list of the installer's files");
            Check(FileCheck.Check(root).Count == 0 && FileCheck.Report(FileCheck.Check(root)).StartsWith("The install is whole"), "file check: whole against its list");
            string before = File.ReadAllText(FileCheck.ListPath(root));
            // a file gone
            File.Delete(Path.Combine(root, "dump", "nro", "lua2cpp_mario.nro"));
            bad = FileCheck.Check(root);
            Check(bad.Count == 2 && bad.All(p => p.Repair == FileRepair.Build) && bad.Any(p => p.Path == "dump/nro/lua2cpp_mario.nro" && !p.Remove), "file check: a fighter's file gone: " + FileCheck.Report(bad));
            Check(FileCheck.Repairs(bad).SequenceEqual(new[] { FileRepair.Build }) && FileCheck.RemoveBroken(root, bad) == 0, "file check: the build step repairs it, nothing to remove");
            Put("dump/nro/lua2cpp_mario.nro", "a fighter's module");
            Check(FileCheck.Check(root).Count == 0, "file check: made again with the same size: whole");
            // a file cut short without being newer than the list: broken, removed before the repair
            string patch = Path.Combine(root, "dump", "modules", "romfs_patch.bin");
            File.WriteAllText(patch, "cut");
            File.SetLastWriteTimeUtc(patch, File.GetLastWriteTimeUtc(FileCheck.ListPath(root)).AddMinutes(-5));
            bad = FileCheck.Check(root);
            Check(bad.Count == 1 && bad[0].Remove && bad[0].Area == "Game data" && bad[0].Repair == FileRepair.Build, "file check: a file of another size: " + FileCheck.Report(bad));
            Check(FileCheck.RemoveBroken(root, bad) == 1 && !File.Exists(patch), "file check: the broken file is removed for the repair");
            Check(FileCheck.Check(root).Count(p => p.Path == "dump/modules/romfs_patch.bin") == 1, "file check: then it is missing, said once");
            // made again by a build (newer than the list, another size): not a problem
            File.WriteAllText(patch, "patch data of a newer build");
            File.SetLastWriteTimeUtc(patch, DateTime.UtcNow.AddMinutes(5));
            Check(FileCheck.Check(root).Count == 0, "file check: a file a later build made again is not a problem");
            // an empty file
            File.WriteAllText(Path.Combine(root, "dump", "fonts", "a.bfttf"), "");
            bad = FileCheck.Check(root);
            Check(bad.Count == 1 && bad[0].Area == "Fonts" && bad[0].Remove, "file check: an empty file: " + FileCheck.Report(bad));
            Put("dump/fonts/a.bfttf");
            // the game's own file moved (data read in place): the set-up, and nothing of the player's is ever removed
            File.Move(xci, xci + ".moved");
            bad = FileCheck.Check(root);
            Check(bad.Count == 1 && bad[0].Repair == FileRepair.Setup && !bad[0].Remove && FileCheck.RemoveBroken(root, bad) == 0, "file check: the game's own file moved: " + FileCheck.Report(bad));
            File.Move(xci + ".moved", xci);
            var fake = new[] { new FileProblem { Area = "x", What = "y", Path = Path.GetRelativePath(root, xci), Repair = FileRepair.Build, Remove = true } };
            Check(FileCheck.RemoveBroken(root, fake) == 0 && File.Exists(xci), "file check: a file outside dump is never removed");
            // the build, a plugin, the shaders
            File.Delete(Path.Combine(root, "build", "ssbu.exe"));
            bad = FileCheck.Check(root);
            Check(bad.Count == 1 && bad[0].Area == "Build" && bad[0].Repair == FileRepair.Build, "file check: no game program built: " + FileCheck.Report(bad));
            Put("build/ssbu.exe", "MZ");
            File.Delete(Path.Combine(root, "dump", "modules", "arcropolis.reloc.bin"));
            File.Delete(Path.Combine(root, "dump", "shadercache", "complete.txt"));
            bad = FileCheck.Check(root);
            Check(bad.Any(p => p.Repair == FileRepair.Plugins) && bad.Any(p => p.Repair == FileRepair.Shaders), "file check: a plugin not built in, shaders not prepared: " + FileCheck.Report(bad));
            Check(FileCheck.Repairs(bad).SequenceEqual(new[] { FileRepair.Build, FileRepair.Plugins, FileRepair.Shaders }), "file check: the repairs in their order");
            Check(File.ReadAllText(FileCheck.ListPath(root)) == before, "file check: a check writes nothing");
            // a repair's files with the game folder gone: said too
            Directory.Move(outside, outside + "x");
            bad = FileCheck.Check(root);
            Check(bad.Any(p => p.Area == "Set-up" && p.Repair == FileRepair.Setup), "file check: the game folder gone: the repair needs the set-up");
            Directory.Move(outside + "x", outside);
            string was = Environment.GetEnvironmentVariable("OPENSMASH_FILE_CHECK");
            Environment.SetEnvironmentVariable("OPENSMASH_FILE_CHECK", "0");
            Check(!FileCheck.Shown, "file check: OPENSMASH_FILE_CHECK=0 hides the button");
            Environment.SetEnvironmentVariable("OPENSMASH_FILE_CHECK", was);
            Check(FileCheck.Shown || was == "0", "file check: shown by default");
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (IOException) { }
            try { Directory.Delete(outside, true); } catch (IOException) { }
        }
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
        Check(SemVer.Parse("0.5.0") > SemVer.Parse("0.5.0-rc.2"), "release newer than its rc");
        Check(SemVer.Parse("0.5.0-rc.10") > SemVer.Parse("0.5.0-rc.2"), "numeric pre-release parts");
        Check(SemVer.Parse("v0.5.0") > SemVer.Parse("0.3.0-alpha.3"), "0.5.0 newer than the alphas");
        Check(SemVer.Parse("0.5.1") > SemVer.Parse("0.5.0"), "patch");
        Check(SemVer.Parse("0.5.0.0").ToString() == "0.5.0", "assembly version form");
        Check(!SemVer.TryParse("main", out _), "a branch is no version");
        Check(Updater.Current.ToString() == "0.5.0-test", "Current from the informational version: " + Updater.Current);

        // ---- a release in a folder (OPENSMASH_RELEASE_DIR)
        string dir = Path.Combine(Path.GetTempPath(), "opensmash-core-test-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(dir);
        try
        {
            byte[] exe = Encoding.ASCII.GetBytes("not really a launcher");
            File.WriteAllBytes(Path.Combine(dir, "OpenSmash.exe"), exe);
            string commit = new string('a', 40);
            string manifest = "{\"schema\":1,\"version\":\"0.5.0\",\"commit\":\"" + commit + "\",\"channel\":\"stable\",\"lobby\":\"lobby.example:1\",\"min_client_level\":1," +
                              "\"launcher\":{\"" + Releases.Platform + "\":{\"file\":\"OpenSmash.exe\",\"sha256\":\"" + Releases.Sha256(exe) + "\"}," +
                              "\"evil\":{\"file\":\"../x.exe\",\"sha256\":\"00\"}}}";
            File.WriteAllText(Path.Combine(dir, Releases.ManifestName), manifest);
            Environment.SetEnvironmentVariable("OPENSMASH_RELEASE_DIR", dir);
            var r = await Releases.LatestAsync("stable");
            Check(r != null && r.Version.ToString() == "0.5.0" && r.Commit == commit && r.Lobby == "lobby.example:1" && r.MinClientLevel == 1, "manifest read");
            Check(r != null && !r.Launcher.ContainsKey("evil"), "a file with a path is not listed");
            Check(Updater.LauncherOutdated(r), "0.5.0 is newer than 0.5.0-test");
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
        Check(svc.Configured, "the repository's build has the accounts service (every launcher built from source can sign in)");
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
        Check(OnlinePass.Expiry("u|n|1800000000|1700000000|d=1001." + new string('f', 64)) == 1800000000, "ticket with a Discord id: expiry");

        // ---- the leaderboard by the lobby ticket (LobbyBoard.cs; tools/nex/board.py's /gsp/me)
        var bme = LobbyBoard.ParseMe("{\"id\":\"abc\",\"rank\":3,\"ranked\":9,\"name\":\"Ann\",\"fighters\":[{\"fighter\":1,\"gsp\":900,\"rank\":4}," +
                                     "{\"fighter\":43,\"gsp\":1500,\"rank\":2},{\"fighter\":\"x\",\"gsp\":1}],\"where\":null}");
        Check(bme != null && bme.Id == "abc" && bme.Rank == 3 && bme.Ranked == 9 && bme.Name == "Ann" && bme.Fighters.Count == 2 &&
              bme.Fighters[0].Fighter == 43 && bme.Fighters[0].Rank == 2, "board: /gsp/me read, highest GSP first");
        Check(LobbyBoard.ParseMe("[1]") == null && LobbyBoard.ParseMe("not json") == null, "board: not an answer");
        var other = LobbyBoard.ParseMe("{\"id\":\"zzz\",\"rank\":1,\"fighters\":[{\"fighter\":1,\"gsp\":1200,\"rank\":1},{\"fighter\":7,\"gsp\":100}]}");
        var merged = LobbyBoard.Merge(new[] { bme, null, other });
        Check(merged.Id == "abc" && merged.Rank == 3 && merged.Fighters.Count == 3 && merged.Fighters[0].Fighter == 43 &&
              merged.Fighters.First(f => f.Fighter == 1).Gsp == 1200 && merged.Fighters.First(f => f.Fighter == 1).Rank == 1,
              "board: lobbies merged, the first one's row and place, each fighter's best");
        Check(await LobbyBoard.MeAsync(new System.Net.Http.HttpClient(), new[] { "127.0.0.1:1" }, "", default) == null, "board: no ticket, nothing asked");
        Check(bme.Shown && LobbyBoard.ParseMe("{\"id\":\"a\",\"show\":false,\"fighters\":[]}")!.Shown == false &&
              LobbyBoard.Merge(new[] { LobbyBoard.ParseMe("{\"show\":false}") }).Shown == false, "board: the shown choice read (none said: shown)");
        {
            // (the show/hide choice: POST /gsp/profile with the ticket header only, a name to show, "" to hide)
            var listener = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
            listener.Start();
            int lport = ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
            var seen = new List<string>();
            var serve = Task.Run(async () =>
            {
                for (int i = 0; i < 2; i++)
                {
                    using var c = await listener.AcceptTcpClientAsync();
                    using var st = c.GetStream();
                    var buf = new byte[8192]; int n = 0, got;
                    string text = "";
                    while ((got = await st.ReadAsync(buf.AsMemory(n, buf.Length - n))) > 0)
                    {
                        n += got; text = System.Text.Encoding.UTF8.GetString(buf, 0, n);
                        int he = text.IndexOf("\r\n\r\n");
                        var m = System.Text.RegularExpressions.Regex.Match(text, @"Content-Length: (\d+)", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                        if (he >= 0 && m.Success && n >= he + 4 + int.Parse(m.Groups[1].Value)) break;
                    }
                    lock (seen) seen.Add(text);
                    byte[] ok = System.Text.Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}");
                    await st.WriteAsync(ok);
                }
            });
            using var hc = new System.Net.Http.HttpClient();
            int took = await LobbyBoard.SetShownAsync(hc, new[] { "127.0.0.1:" + lport }, "u|n|1800000000." + new string('f', 64), false, default);
            int took2 = await LobbyBoard.SetShownAsync(hc, new[] { "127.0.0.1:" + lport }, "u|n|1800000000." + new string('f', 64), true, default);
            await serve; listener.Stop();
            Check(took == 1 && took2 == 1 && seen.Count == 2 && seen[0].StartsWith("POST /gsp/profile ") &&
                  seen[0].Contains("X-OpenSmash-Ticket: u|n|1800000000.") && seen[0].Contains("\"name\":\"\"") &&
                  !seen[0].Contains("X-OpenSmash-Player") && !seen[1].Contains("\"name\":\"\""),
                  "board: show/hide sent with the ticket alone (\"\" hides, a name shows)");
            Check(await LobbyBoard.SetShownAsync(hc, new[] { "127.0.0.1:1" }, "", true, default) == 0, "board: no ticket, no show/hide sent");
        }
        string tkroot = Path.Combine(Path.GetTempPath(), "os-core-ticket-" + Guid.NewGuid().ToString("N"));
        try
        {
            Check(LobbyBoard.StoredTicket(tkroot) == null, "board: no stored ticket");
            Directory.CreateDirectory(Path.Combine(tkroot, "build", "cache"));
            File.WriteAllText(Path.Combine(tkroot, "build", "cache", "online_pass.txt"), "u|n|1800000000." + new string('f', 64) + "\n");
            Check(LobbyBoard.StoredTicket(tkroot, 60, 1700000000) == "u|n|1800000000." + new string('f', 64), "board: the stored ticket, still fresh");
            Check(LobbyBoard.StoredTicket(tkroot, 60, 1799999990) == null, "board: a stored ticket about to expire is not used");
        }
        finally { try { Directory.Delete(tkroot, true); } catch { } }
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string dirty = "at " + home + "\\x.txt by " + Environment.UserName + " from 93.184.216.34 room 10.13.0.5 mail a.b@example.com pass " +
                       "00000000-0000-4000-8000-000000000001|Alice|1800000000." + new string('f', 64) + " at C:\\Games\\OpenSmash\\build";
        string clean = CrashReport.Clean(dirty, "C:\\Games\\OpenSmash");
        Check(!clean.Contains(home) && !clean.Contains("93.184.216.34") && clean.Contains("10.13.0.5") && !clean.Contains("example.com") &&
              clean.Contains("<pass>") && clean.Contains("<install>\\build"), "report cleaned: " + clean);

        // ---- the session's performance summary
        string perf = "{\"schema\":1,\"clean_exit\":true,\"session_s\":300,\"frames\":18000,\"ft_p50\":16.62,\"ft_p99\":21.5,\"stutters\":3," +
                      "\"hist\":{\"<8\":0,\"<12\":1,\">=100\":2},\"rb_rollbacks\":120,\"path\":\"C:/Users/someone\",\"name\":\"someone\"}";
        var pp = PerfReport.Payload(perf, new Dictionary<string, object> { ["cpu"] = "Test CPU", ["ram_gb"] = 16.0 }, "[vulkan] device: Test GPU (driver 1)\nrenderer: vulkan\n", "0.5.0", "abc");
        Check(pp != null && (double)pp["frames"] == 18000 && (string)pp["cpu"] == "Test CPU" && (string)pp["gpu"] == "Test GPU" && (string)pp["renderer"] == "vulkan" &&
              (bool)pp["crashed"] == false && !pp.ContainsKey("path") && !pp.ContainsKey("name") && ((Dictionary<string, long>)pp["hist"])[">=100"] == 2, "performance summary payload");
        Check(!System.Text.Json.JsonSerializer.Serialize(pp).Contains("someone"), "performance summary: unknown fields dropped");
        Check(PerfReport.Payload(perf.Replace("\"session_s\":300", "\"session_s\":20"), null, null, "0.5.0", "abc") == null, "performance summary: short sessions are not sent");
        Check(PerfReport.Payload("not json", null, null, "0.5.0", "abc") == null, "performance summary: not a summary");
        var pc = PerfReport.Pc();
        Check(pc.ContainsKey("cpu_threads") && !System.Text.Json.JsonSerializer.Serialize(pc).Contains(Environment.UserName, StringComparison.OrdinalIgnoreCase), "this PC's kind has no user name");
        string troot = Path.Combine(Path.GetTempPath(), "os-core-perf-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(troot);
        Check(PerfReport.Enabled(troot), "performance summaries: on by default");
        PerfReport.SetEnabled(troot, false);
        Check(!PerfReport.Enabled(troot), "performance summaries: off in the settings");
        Directory.Delete(troot, true);

        // "Save disk space" (DiskSpace.cs): config\save_disk.txt, off by default; tools/build_game.py reads the same file
        string droot = Path.Combine(Path.GetTempPath(), "os-core-disk-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(droot);
        Check(!DiskSpace.Saving(droot), "save disk space: off by default (no file)");
        DiskSpace.SetSaving(droot, true);
        Check(DiskSpace.Saving(droot) && File.ReadAllText(Path.Combine(droot, "config", "save_disk.txt")) == "on\n", "save disk space: on is written to config\\save_disk.txt");
        DiskSpace.SetSaving(droot, false);
        Check(!DiskSpace.Saving(droot) && File.ReadAllText(Path.Combine(droot, "config", "save_disk.txt")) == "off\n", "save disk space: off again");
        File.WriteAllText(Path.Combine(droot, "config", "save_disk.txt"), "ON \r\n");
        Check(DiskSpace.Saving(droot), "save disk space: read whatever the case and line ending");
        Check(DiskSpace.DataInPlace == "SSBU_DATA_IN_PLACE" && DiskSpace.Trim == "SSBU_TRIM" && DiskSpace.TrimWhat == "c,nsp,map", "save disk space: the build's own switches");
        Directory.Delete(droot, true);

        // the players online of a region (OnlineCount.cs): the accounts service's lobby_status row, fresh rows only
        var at = new DateTimeOffset(2026, 11, 17, 12, 0, 0, TimeSpan.Zero);
        Check(OnlineCount.Path("na-east") == "/rest/v1/lobby_status?select=players,rooms,updated_at&product=eq.opensmash&region=eq.na-east&limit=1", "online count: the region's row of this product is asked");
        Check(OnlineCount.Parse("[{\"players\":12,\"rooms\":3,\"updated_at\":\"2026-11-17T11:58:30.123456+00:00\"}]", at) == (12, 3), "online count: a fresh row's players and rooms");
        Check(OnlineCount.Parse("[{\"players\":0,\"rooms\":0,\"updated_at\":\"2026-11-17T12:00:20+00:00\"}]", at) == (0, 0), "online count: nobody online is a count too (and a clock a little behind)");
        Check(OnlineCount.Parse("[{\"players\":12,\"rooms\":3,\"updated_at\":\"2026-11-17T11:44:59+00:00\"}]", at) == null, "online count: a row older than 15 minutes is unknown");
        Check(OnlineCount.Parse("[{\"players\":12,\"rooms\":3,\"updated_at\":\"2026-11-17T13:58:30+02:00\"}]", at) == (12, 3), "online count: the time in another zone");
        Check(OnlineCount.Parse("[]", at) == null, "online count: no row for the region (the older single row only): none, the lobby's own page is shown");
        Check(OnlineCount.Parse("{\"message\":\"column lobby_status.region does not exist\"}", at) == null && OnlineCount.Parse("<html>", at) == null
              && OnlineCount.Parse("[{\"players\":\"many\",\"updated_at\":\"2026-11-17T12:00:00Z\"}]", at) == null && OnlineCount.Parse("[{\"players\":5}]", at) == null, "online count: anything else is none");
        Check(OnlineCount.Parse("[{\"players\":-4,\"updated_at\":\"2026-11-17T12:00:00Z\"}]", at) == (0, 0), "online count: never below zero, rooms optional");
        Environment.SetEnvironmentVariable("OPENSMASH_ONLINE_COUNT", "lobby");
        bool lobbyOnly = !OnlineCount.Wanted;
        Environment.SetEnvironmentVariable("OPENSMASH_ONLINE_COUNT", null);
        Check(lobbyOnly && OnlineCount.Wanted, "online count: OPENSMASH_ONLINE_COUNT=lobby asks the lobby's own page only");
        Check(Array.TrueForAll(ServerRegion.All, r => r.Id == "eu" || r.Id == "na-east"), "online count: the regions' ids are the table's");

        FileChecks();

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
