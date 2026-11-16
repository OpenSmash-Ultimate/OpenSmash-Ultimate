// SPDX-License-Identifier: GPL-3.0-or-later
// Updates from the release channel (Releases.cs), the way SSBU Arena's launcher does them:
// - a newer launcher: its file is downloaded, checked against its SHA-256 and put in place of the running one (renamed
//   to .old, which Windows allows), then started again with --updated. No .NET SDK needed on the player's PC;
// - another commit than the installed one (tools/source_version.txt, which the installer's updater writes): the
//   installer brings the sources to the release's commit (install_windowsmash.ps1 -UpdateOnly -UpdateCommit <sha>) and
//   rebuilds what the change needs (-BuildAfterUpdate);
// - not installed yet: the release commit's sources are unpacked beside the launcher (FirstInstallAsync).
// A git checkout (a developer's copy) is never changed; OPENSMASH_UPDATE=off skips everything, =force checks a git checkout too.
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public static class Updater
    {
        static string Mode => (Environment.GetEnvironmentVariable("OPENSMASH_UPDATE") ?? "").Trim().ToLowerInvariant();
        public static bool Off => Mode == "off";

        /// <summary>this launcher's version (the release workflow builds it with -p:Version=X.Y.Z); 0.0.0 for a local build</summary>
        public static SemVer Current
        {
            get
            {
                var a = Assembly.GetEntryAssembly() ?? Assembly.GetExecutingAssembly();
                string info = a.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
                if (SemVer.TryParse(info, out var v)) return v;
                var n = a.GetName().Version;
                return n == null ? new SemVer() : new SemVer { Major = n.Major, Minor = n.Minor, Patch = Math.Max(n.Build, 0) };
            }
        }

        public static string ExePath => Environment.ProcessPath ?? Process.GetCurrentProcess().MainModule.FileName;

        /// <summary>a developer's git checkout: nothing is changed there unless OPENSMASH_UPDATE=force</summary>
        public static bool DevCopy(string root) => root != null && Directory.Exists(Path.Combine(root, ".git")) && Mode != "force";

        /// <summary>the channel this install follows: config/update_channel.txt "testing", else "stable"</summary>
        public static string Channel(string root)
        {
            try
            {
                string f = Path.Combine(root ?? "", "config", "update_channel.txt");
                if (root != null && File.Exists(f) && File.ReadAllText(f).Trim().Equals("testing", StringComparison.OrdinalIgnoreCase)) return "testing";
            }
            catch { }
            return "stable";
        }

        public static void SetChannel(string root, string channel)
        {
            Directory.CreateDirectory(Path.Combine(root, "config"));
            File.WriteAllText(Path.Combine(root, "config", "update_channel.txt"), (channel == "testing" ? "testing" : "stable") + "\n");
        }

        /// <summary>the commit the sources are at: tools/source_version.txt "&lt;sha&gt; &lt;date&gt;" (the installer's updater and
        /// the source archives write it); null when unknown</summary>
        public static string InstalledCommit(string root)
        {
            try
            {
                string f = Path.Combine(root, "tools", "source_version.txt");
                if (!File.Exists(f)) return null;
                string w = File.ReadAllText(f).Trim().Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault();
                return w != null && w.Length == 40 && w.All(Uri.IsHexDigit) ? w.ToLowerInvariant() : null;
            }
            catch { return null; }
        }

        /// <summary>an update of the game that began and did not end well (the launcher writes it, the installer's success removes it)</summary>
        public static string PendingMarker(string root) => Path.Combine(root, "config", "update-game-build.pending");

        public static bool LauncherOutdated(Release r) =>
            r != null && r.Launcher.ContainsKey(Releases.Platform) && r.Version > Current && !Off;

        public static bool GameOutdated(Release r, string root) =>
            r != null && root != null && !Off && !DevCopy(root) && (InstalledCommit(root) != r.Commit || File.Exists(PendingMarker(root)));

        /// <summary>what the update check found and did: build\launcher_update.log (appended; trimmed past 256 KB)</summary>
        public static void Log(string root, string s)
        {
            try
            {
                string dir = root != null ? Path.Combine(root, "build") : Path.GetDirectoryName(ExePath);
                Directory.CreateDirectory(dir);
                string f = Path.Combine(dir, "launcher_update.log");
                if (File.Exists(f) && new FileInfo(f).Length > 256 * 1024) File.Delete(f);
                File.AppendAllText(f, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss ") + s + Environment.NewLine);
            }
            catch { }
        }

        /// <summary>leftovers of the last self-update</summary>
        public static void CleanUp()
        {
            foreach (var p in new[] { ExePath + ".old", ExePath + ".new" })
                try { if (File.Exists(p)) File.Delete(p); } catch { }
        }

        /// <summary>the release's launcher in place of this one (and of the copies listed); the caller then starts it again
        /// (StartUpdated) and exits</summary>
        public static async Task ReplaceLauncherAsync(Release r, CancellationToken cancel = default, IProgress<double> progress = null, params string[] otherCopies)
        {
            byte[] exe = await Releases.DownloadCheckedAsync(r.Launcher[Releases.Platform], cancel, progress);
            string me = ExePath;
            File.WriteAllBytes(me + ".new", exe);
            if (File.Exists(me + ".old")) File.Delete(me + ".old");
            File.Move(me, me + ".old");
            File.Move(me + ".new", me);
            if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(me, (UnixFileMode)0b111_101_101);
            foreach (var other in otherCopies.Where(o => o != null && File.Exists(o) && !string.Equals(Path.GetFullPath(o), Path.GetFullPath(me), StringComparison.OrdinalIgnoreCase)))
                try { File.WriteAllBytes(other, exe); } catch { }
        }

        public static void StartUpdated(string args = "--updated")
        {
            Process.Start(new ProcessStartInfo(ExePath, args) { UseShellExecute = false });
        }

        /// <summary>first start of a downloaded launcher: the release commit's sources unpacked into root (the installer then
        /// asks for the game files). Paths are checked: nothing outside root is written.</summary>
        public static async Task FirstInstallAsync(Release r, string root, Action<string> say, CancellationToken cancel = default, IProgress<double> progress = null)
        {
            say?.Invoke("Downloading OpenSmash " + r.Version + "...");
            byte[] zip = await Releases.GetAsync(r.SourceZipUrl, cancel, null, progress);
            say?.Invoke("Unpacking...");
            Unpack(zip, root);
            // (the commit, as the installer's own updater records it: the next update knows where the sources are)
            File.WriteAllText(Path.Combine(root, "tools", "source_version.txt"), r.Commit + " " + DateTime.UtcNow.ToString("yyyy-MM-dd") + "\n");
            WriteLobby(r, root);
        }

        /// <summary>a GitHub source archive (one top folder) into root</summary>
        public static void Unpack(byte[] zip, string root)
        {
            string full = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            using var z = new ZipArchive(new MemoryStream(zip));
            foreach (var e in z.Entries)
            {
                int cut = e.FullName.IndexOf('/');
                string rel = cut < 0 ? "" : e.FullName.Substring(cut + 1);
                if (rel == "" || rel.EndsWith("/")) continue;
                if (rel.Split('/').Any(p => p == ".." || p == "") || rel.Contains(':') || rel.Contains('\\'))
                    throw new InvalidDataException("unsafe path in the download: " + e.FullName);
                string dst = Path.GetFullPath(Path.Combine(root, rel.Replace('/', Path.DirectorySeparatorChar)));
                if (!dst.StartsWith(full, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("unsafe path in the download: " + e.FullName);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                e.ExtractToFile(dst, true);
            }
        }

        /// <summary>the release's official lobby for the game (runtime/net_relay.c reads config/official_lobby.txt)</summary>
        public static void WriteLobby(Release r, string root)
        {
            if (r?.Lobby == null || r.Lobby.Any(c => char.IsWhiteSpace(c) || char.IsControl(c)) || r.Lobby.Length > 200) return;
            try
            {
                Directory.CreateDirectory(Path.Combine(root, "config"));
                string f = Path.Combine(root, "config", "official_lobby.txt");
                if (!File.Exists(f) || File.ReadAllText(f).Trim() != r.Lobby) File.WriteAllText(f, r.Lobby + "\n");
            }
            catch { }
        }
    }
}
