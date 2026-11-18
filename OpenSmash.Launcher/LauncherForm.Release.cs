// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.0) Updates from the release channel (OpenSmash.Core.Releases) instead of main's newest commit:
// a player's copy (not a git checkout) follows config/update_channel.txt: "stable" (default: GitHub's latest release,
// never a pre-release), "testing" (pre-releases too) or "main" (the old way: every commit of main, rebuilding the
// launcher with the .NET SDK). A newer launcher replaces this exe by itself (SHA-256 checked, no SDK needed); the game
// is brought to the release's commit by the installer (OPENSMASH_UPDATE_COMMIT) and rebuilt where it changed.
using System.Diagnostics;
using OpenSmash.Core;

namespace OpenSmash.Launcher
{
    partial class LauncherForm
    {
        Release? _release;
        static string? s_updateCommit;   // (RunPowerShellUpdate: the release's commit for the installer, null on main)
        static bool s_followMain;        // (the "main" channel: said to the installer too)
        static string? s_releaseTag;     // (...and its tag: the installer builds the game DLL and takes the release's runtime)

        /// <summary>(Program, before the installer's first build) OPENSMASH_RELEASE_TAG for the installer when this copy's
        /// sources are a release's (its commit): the game DLL and the release's prebuilt runtime</summary>
        public static void NameReleaseForInstaller(string repo)
        {
            try
            {
                if (Updater.Off || Directory.Exists(Path.Combine(repo, ".git"))) return;
                string channel = File.Exists(Path.Combine(repo, "config", "update_channel.txt")) ? Updater.Channel(repo) : "stable";
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
                var r = Releases.LatestAsync(channel, cts.Token).GetAwaiter().GetResult();
                if (r != null && r.Commit == Updater.InstalledCommit(repo))
                {
                    Environment.SetEnvironmentVariable("OPENSMASH_RELEASE_TAG", r.Tag);
                    Updater.WriteLobby(r, repo);
                }
            }
            catch { }
        }

        string ChannelSetting()
        {
            try
            {
                // (the creator's opt-in to main, issue #329: the variable, or "main" in tools\update_channel.txt)
                string env = Environment.GetEnvironmentVariable("OPENSMASH_UPDATE_CHANNEL") ?? "";
                string old = Path.Combine(_repo, "tools", "update_channel.txt");
                if (env.Trim().Equals("main", StringComparison.OrdinalIgnoreCase)) return "main";
                if (env.Length == 0 && File.Exists(old) && File.ReadAllText(old).Trim().Equals("main", StringComparison.OrdinalIgnoreCase)) return "main";
                string f = Path.Combine(_repo, "config", "update_channel.txt");
                string v = File.Exists(f) ? File.ReadAllText(f).Trim().ToLowerInvariant() : "";
                return v == "testing" || v == "main" ? v : "stable";
            }
            catch { return "stable"; }
        }

        /// <summary>this copy follows releases: not a git checkout, not on the "main" channel, updates not turned off</summary>
        bool ReleaseMode => !Updater.Off && !Directory.Exists(Path.Combine(_repo, ".git")) && ChannelSetting() != "main";

        /// <summary>CheckForUpdatesAsync's release form: the same states and texts</summary>
        async Task CheckReleaseAsync()
        {
            string local = await Task.Run(() => Updater.InstalledCommit(_repo) ?? "");
            _updateLocal = local;
            string channel = ChannelSetting();
            _release = await LatestReleaseAsync(channel);
            if (_release == null)
            {
                _updateState = "uptodate";
                _updateBehind = 0;
                _updateMessage = "No OpenSmash release on this channel yet: nothing to update.";
                _updateDetail = "channel: " + channel;
                return;
            }
            _updateHead = _release.Commit;
            Updater.WriteLobby(_release, _repo);
            bool launcher = Updater.LauncherOutdated(_release), game = Updater.GameOutdated(_release, _repo);
            Updater.Log(_repo, $"check ({channel}): release {_release.Version} {_release.Commit[..10]}, installed {(local.Length > 0 ? local[..Math.Min(10, local.Length)] : "?")}, " +
                               $"launcher {Updater.Current}{(launcher ? " (older)" : "")}");
            if (launcher || game)
            {
                _updateState = "available";
                _updateBehind = 1;
                _updateMessage = UiLocale.Interpolate($"OpenSmash {_release.Version} is available.");
                _updateDetail = (channel == "testing" ? "testing channel" : "stable channel") + (_release.Page != null ? "   ·   " + _release.Page : "");
            }
            else
            {
                _updateState = "uptodate";
                _updateBehind = 0;
                _updateMessage = UiLocale.Interpolate($"OpenSmash {_release.Version} is up to date.");
                _updateDetail = channel == "testing" ? "testing channel" : "";
            }
        }

        static async Task<Release?> LatestReleaseAsync(string channel)
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            return await Releases.LatestAsync(channel, cts.Token);
        }

        /// <summary>a newer launcher first: downloaded, checked, put in place of this exe and started again. true when this
        /// launcher must close (the new one is starting).</summary>
        async Task<bool> SelfUpdateLauncherAsync()
        {
            if (_release == null || !Updater.LauncherOutdated(_release)) return false;
            SetLoad("UPDATING THE LAUNCHER", -1);
            try
            {
                string atRoot = Path.Combine(_repo, "OpenSmash.exe");
                await Updater.ReplaceLauncherAsync(_release, default, null, atRoot);
                Updater.Log(_repo, "launcher replaced by " + _release.Version);
                Updater.StartUpdated("--updated");
                return true;
            }
            catch (Exception ex)
            {
                Updater.Log(_repo, "launcher not replaced: " + ex.Message);
                UiLocale.Message(UiLocale.Text("The new launcher could not be installed: ") + ex.Message, "OpenSmash");
                return false;
            }
            finally { ClearLoad(); }
        }
    }
}
