using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Launcher
{
    // ============================================================================
    // ProjectSetup: what OpenSmash.exe needs when it runs on its own (the release
    // download) instead of from a copy of the project. It finds the project folder
    // (the exe's own, or the one remembered from the first start) and gets a new
    // one: GitHub's ZIP of main, the download the Update button's installer step
    // makes (tools\install_windowsmash.ps1, Update-FromZip), checked the same way,
    // unpacked into an empty folder. Its download is the one the mod browser uses.
    // ============================================================================
    static class ProjectSetup
    {
        public const string Repo = "OpenSmash-Ultimate/OpenSmash-Ultimate";
        public const string MainZipUrl = "https://codeload.github.com/" + Repo + "/zip/refs/heads/main";
        const string WorkName = ".opensmash-download";   // inside the chosen folder while it is filled; removed after

        // Downloads can be hundreds of megabytes, so they get their own client with a
        // generous timeout instead of the 15s one used for small API calls.
        public static readonly HttpClient DownloadClient = new HttpClient
        {
            Timeout = TimeSpan.FromMinutes(30),
        };

        // GET url into destination; onProgress(bytes so far, total or -1 when the server does not say).
        public static async Task DownloadAsync(string url, string destination, Func<long, long, Task> onProgress, CancellationToken ct = default)
        {
            using var response = await DownloadClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
            response.EnsureSuccessStatusCode();

            long total = response.Content.Headers.ContentLength ?? -1;
            await using var source = await response.Content.ReadAsStreamAsync(ct);
            await using var target = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None, 81920, true);

            var buffer = new byte[81920];
            long received = 0;
            int read;

            while ((read = await source.ReadAsync(buffer, ct)) > 0)
            {
                await target.WriteAsync(buffer.AsMemory(0, read), ct);
                received += read;
                await onProgress(received, total);
            }
        }

        // ---------------------------------------------------------------------------
        // Where the project is
        public static bool IsProject(string dir)
        {
            try { return dir != "" && File.Exists(Path.Combine(dir, "tools", "install_windowsmash.ps1")); }
            catch { return false; }
        }

        // The folder the exe counts as its own: where it lives, or the repo root above a build output folder.
        public static string ExeRoot(string baseDir)
        {
            string repo = Path.GetFullPath(baseDir).TrimEnd('\\');
            while (repo.EndsWith("OpenSmash.Launcher", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("bin", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("Debug", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("Release", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("publish", StringComparison.OrdinalIgnoreCase))
            {
                repo = Path.GetFullPath(Path.Combine(repo, ".."));
            }
            return repo;
        }

        // The exe's folder when it is a project, else the folder the first start set up (or chose), else null.
        public static string? FindProject(string baseDir)
        {
            string root = ExeRoot(baseDir);
            if (IsProject(root)) return root;
            string remembered = Remembered();
            return IsProject(remembered) ? remembered : null;
        }

        // %LOCALAPPDATA%\OpenSmash\project_folder.txt; OPENSMASH_SETTINGS_DIR moves it (tests).
        static string RememberFile()
        {
            string? dir = Environment.GetEnvironmentVariable("OPENSMASH_SETTINGS_DIR");
            if (string.IsNullOrEmpty(dir)) dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenSmash");
            return Path.Combine(dir, "project_folder.txt");
        }

        public static string Remembered()
        {
            try
            {
                string file = RememberFile();
                if (!File.Exists(file)) return "";
                return File.ReadLines(file, Encoding.UTF8).FirstOrDefault()?.Trim().Trim('"') ?? "";
            }
            catch { return ""; }
        }

        public static void Remember(string dir)
        {
            string file = RememberFile();
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            File.WriteAllText(file, Path.GetFullPath(dir) + Environment.NewLine, new UTF8Encoding(false));
        }

        // ---------------------------------------------------------------------------
        // The folder a new copy goes into
        public enum FolderKind { Missing, Empty, Project, Other }

        // Empty also when it holds only this exe (or a download that stopped): the user put OpenSmash.exe in a
        // folder of its own. Anything else in it is the user's, and the folder is not used without asking.
        public static FolderKind Inspect(string dir)
        {
            try
            {
                if (File.Exists(dir)) return FolderKind.Other;
                if (!Directory.Exists(dir)) return FolderKind.Missing;
                if (IsProject(dir)) return FolderKind.Project;
                string self = Path.GetFileName(Environment.ProcessPath ?? "OpenSmash.exe");
                foreach (string entry in Directory.EnumerateFileSystemEntries(dir))
                {
                    string name = Path.GetFileName(entry);
                    bool mine = name.Equals(WorkName, StringComparison.OrdinalIgnoreCase)
                        || (File.Exists(entry) && (name.Equals(self, StringComparison.OrdinalIgnoreCase) || name.Equals("OpenSmash.exe", StringComparison.OrdinalIgnoreCase)));
                    if (!mine) return FolderKind.Other;
                }
                return FolderKind.Empty;
            }
            catch { return FolderKind.Other; }
        }

        // An exe alone in its folder installs there; otherwise into a new OpenSmash folder beside it.
        public static string DefaultFolder(string exeDir)
        {
            string here = Path.GetFullPath(exeDir).TrimEnd('\\');
            if (Inspect(here) == FolderKind.Empty) return here;
            string sub = Path.Combine(here, "OpenSmash");
            for (int i = 2; Inspect(sub) is FolderKind.Other; i++) sub = Path.Combine(here, "OpenSmash " + i);
            return sub;
        }

        // ---------------------------------------------------------------------------
        // Getting the project: source is a URL (GitHub's ZIP) or a ZIP already on disk.
        // onProgress(phase "download" | "extract", done, total or -1). Throws with a sentence for the user;
        // on any failure the folder is left as it was found (made by this, it is removed again).
        public static async Task InstallAsync(string folder, string source, Action<string, long, long> onProgress, CancellationToken ct)
        {
            folder = Path.GetFullPath(folder).TrimEnd('\\');
            var kind = Inspect(folder);
            if (kind == FolderKind.Other) throw new IOException("This folder has other files in it, so OpenSmash was not put there. Choose an empty folder or a new one.");
            if (kind == FolderKind.Project) { Remember(folder); return; }
            bool made = kind == FolderKind.Missing;
            string work = Path.Combine(folder, WorkName);
            try
            {
                Directory.CreateDirectory(folder);
                if (Directory.Exists(work)) Directory.Delete(work, true);
                Directory.CreateDirectory(work);

                string zip = source;
                if (!File.Exists(source))
                {
                    zip = Path.Combine(work, "OpenSmash-Ultimate-main.zip");
                    string partial = zip + ".partial";
                    await DownloadAsync(source, partial, (got, total) => { onProgress("download", got, total); return Task.CompletedTask; }, ct);
                    File.Move(partial, zip);
                }

                string extract = Path.Combine(work, "extract");
                await Task.Run(() => Extract(zip, extract, onProgress, ct), ct);

                // the same check as Update-FromZip: one top folder, and it is the project
                var top = Directory.GetDirectories(extract);
                if (top.Length != 1 || Directory.GetFiles(extract).Length != 0 || !File.Exists(Path.Combine(top[0], "tools", "recomp.py")) || !IsProject(top[0]))
                    throw new InvalidDataException("The download does not look complete, so nothing was set up. Try again.");

                // tools\ last: until it is in place the folder does not count as a project
                var entries = Directory.EnumerateFileSystemEntries(top[0])
                    .OrderBy(p => Path.GetFileName(p).Equals("tools", StringComparison.OrdinalIgnoreCase) ? 1 : 0).ToList();
                foreach (string from in entries)
                {
                    ct.ThrowIfCancellationRequested();
                    string to = Path.Combine(folder, Path.GetFileName(from));
                    if (File.Exists(to) || Directory.Exists(to)) continue;   // (only this exe can be there: kept)
                    if (Directory.Exists(from)) Directory.Move(from, to);
                    else File.Move(from, to);
                }
                Directory.Delete(work, true);
            }
            catch
            {
                try { if (Directory.Exists(work)) Directory.Delete(work, true); } catch { }
                try { if (made && Directory.Exists(folder) && !Directory.EnumerateFileSystemEntries(folder).Any()) Directory.Delete(folder); } catch { }
                throw;
            }

            CopyLauncher(folder);
            Remember(folder);
        }

        // The shortcuts, the installer's and Play SSBU.bat start <folder>\OpenSmash.exe: a copy of this one when
        // there is none (this may be "OpenSmash Setup.exe"). A failed copy is no error: the installer downloads it.
        public static void CopyLauncher(string folder)
        {
            try
            {
                string self = Environment.ProcessPath ?? "";
                string copy = Path.Combine(folder, "OpenSmash.exe");
                if (self.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && File.Exists(self) && !File.Exists(copy)) File.Copy(self, copy);
            }
            catch { }
        }

        static void Extract(string zip, string destination, Action<string, long, long> onProgress, CancellationToken ct)
        {
            string root = Path.GetFullPath(destination).TrimEnd('\\') + "\\";
            Directory.CreateDirectory(root);
            using var archive = ZipFile.OpenRead(zip);
            long total = archive.Entries.Count, done = 0;
            foreach (var entry in archive.Entries)
            {
                ct.ThrowIfCancellationRequested();
                string path = Path.GetFullPath(Path.Combine(root, entry.FullName.Replace('/', '\\')));
                if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The download has a file that points outside its folder, so nothing was set up.");
                if (entry.FullName.EndsWith("/")) Directory.CreateDirectory(path);
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    entry.ExtractToFile(path, true);
                }
                done++;
                if (done % 25 == 0 || done == total) onProgress("extract", done, total);
            }
        }

        // ---------------------------------------------------------------------------
        // Shortcuts: OpenSmash (the launcher, with its own icon) and Play SSBU (OpenSmash.exe --play: the game
        // straight away, or the installer while it is not built; WindowSmash's icon, the game window's).
        // Into the desktop and a Start menu folder named OpenSmash, each left out when its folder is "". Returns the files.
        public static List<string> MakeShortcuts(string folder, string desktop, string startMenu)
        {
            folder = Path.GetFullPath(folder).TrimEnd('\\');
            string launcher = Path.Combine(folder, "OpenSmash.exe");
            string gameIcon = Path.Combine(folder, "docs", "assets", "windowsmash.ico");
            string appIcon = File.Exists(launcher) ? launcher + ",0" : Path.Combine(folder, "media", "opensmash.ico");
            if (!File.Exists(gameIcon)) gameIcon = appIcon;
            else gameIcon += ",0";
            var made = new List<string>();
            var places = new List<string>();
            if (desktop != "") places.Add(desktop);
            if (startMenu != "") places.Add(Path.Combine(startMenu, "OpenSmash"));
            if (places.Count == 0) return made;
            dynamic shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell")!)!;
            foreach (string dir in places)
            {
                Directory.CreateDirectory(dir);
                foreach (var (name, args, icon, what) in new[]
                {
                    ("OpenSmash.lnk", "", appIcon, "OpenSmash: play, settings and mods"),
                    ("Play SSBU.lnk", "--play", gameIcon, "Super Smash Bros. Ultimate (OpenSmash)"),
                })
                {
                    string lnk = Path.Combine(dir, name);
                    dynamic s = shell.CreateShortcut(lnk);
                    s.TargetPath = launcher;
                    s.Arguments = args;
                    s.WorkingDirectory = folder;
                    s.IconLocation = icon;
                    s.Description = what;
                    s.Save();
                    made.Add(lnk);
                }
            }
            return made;
        }

        public static string DesktopDir() => Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        public static string StartMenuDir() => Environment.GetFolderPath(Environment.SpecialFolder.Programs);

        // ---------------------------------------------------------------------------
        // OpenSmash.exe --fetch-project <folder> [--from <zip or url>]: InstallAsync without a window, for tests.
        // Prints "ok <folder>" (exit 0), "refused <why>" (exit 2) or "error <why>" (exit 1).
        // OpenSmash.exe --find-project: prints the folder Program would use, or nothing.
        // OpenSmash.exe --make-shortcuts <folder> --desktop <dir> [--start-menu <dir>]: prints each shortcut made.
        public static bool IsHeadless(string[] args) =>
            args.Any(a => a.Equals("--fetch-project", StringComparison.OrdinalIgnoreCase) || a.Equals("--find-project", StringComparison.OrdinalIgnoreCase)
                       || a.Equals("--make-shortcuts", StringComparison.OrdinalIgnoreCase));

        public static int RunHeadless(string[] args)
        {
            static string? After(string[] a, string flag)
            {
                int i = Array.FindIndex(a, x => x.Equals(flag, StringComparison.OrdinalIgnoreCase));
                return i >= 0 && i + 1 < a.Length ? a[i + 1] : null;
            }
            if (args.Contains("--find-project", StringComparer.OrdinalIgnoreCase))
            {
                Console.Out.WriteLine(FindProject(AppContext.BaseDirectory) ?? "");
                return 0;
            }
            if (After(args, "--make-shortcuts") is string target)
            {
                // (no default desktop here: a test must never write to the real one)
                string? desktop = After(args, "--desktop");
                if (string.IsNullOrEmpty(desktop)) { Console.Out.WriteLine("error --make-shortcuts needs --desktop <dir>"); return 1; }
                try
                {
                    foreach (string lnk in MakeShortcuts(target, desktop, After(args, "--start-menu") ?? "")) Console.Out.WriteLine("made " + lnk);
                    return 0;
                }
                catch (Exception ex) { Console.Out.WriteLine("error " + ex.Message); return 1; }
            }
            string folder = After(args, "--fetch-project") ?? "";
            if (folder == "") { Console.Out.WriteLine("error --fetch-project needs a folder"); return 1; }
            if (Inspect(folder) == FolderKind.Other) { Console.Out.WriteLine("refused the folder has other files"); return 2; }
            try
            {
                string last = "";
                InstallAsync(folder, After(args, "--from") ?? MainZipUrl, (phase, done, total) =>
                {
                    string line = phase + (total > 0 ? $" {done * 100 / total}%" : $" {done / 1048576} MB");
                    if (line != last) { last = line; Console.Out.WriteLine(line); }
                }, CancellationToken.None).GetAwaiter().GetResult();
                Console.Out.WriteLine("ok " + Path.GetFullPath(folder));
                return 0;
            }
            catch (Exception ex)
            {
                Console.Out.WriteLine("error " + ex.Message.Replace("\r", " ").Replace("\n", " "));
                return 1;
            }
        }
    }
}
