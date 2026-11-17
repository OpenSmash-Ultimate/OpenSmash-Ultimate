// SPDX-License-Identifier: GPL-3.0-or-later
// "Verify game files" (the launcher's status strip; SSBU Arena's "Check and repair"): is this install whole?
// Nothing is changed by a check. What is looked at, all of it what the installer made (never the game's own files'
// content, which is not hashed: a check takes a second or two):
//   - the set-up's record (dump\build_folders.txt: where the game files, keys and firmware were);
//   - the game's program (dump\exefs), the fighters (dump\nro) and the fonts (dump\fonts, or dump\fonts.none);
//   - the game data (dump\modules\romfs.cfg and every file it reads: the installer's own, or the game's own file
//     where the data is read in place);
//   - the list the launcher keeps of those files with their sizes (dump\files_check.tsv, written after a build the
//     launcher ran, or made from the files as they are at the first check): a file gone, empty, or of another size
//     without being newer than the list (a copy cut short, a disk fault, an antivirus);
//   - the build (build\ssbu.exe, the Vulkan shader translator beside it), the plugins built in
//     (dump\skyline\plugins.txt), and whether the shaders are prepared (dump\shadercache\complete.txt).
// Each problem says how it is repaired: Build (the installer's build step, which makes again only what is missing:
// tools\install_windowsmash.ps1 -BuildAfterUpdate), Plugins (the plugins built in again), Shaders (Prepare Shaders),
// Setup (the game files are not where they were: the set-up asks for them again), or None (a note).
// A repair removes only files of the installer's own that were found broken (RemoveBroken), never one outside dump\.
// OPENSMASH_FILE_CHECK=0: the launcher does not show the button.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace OpenSmash.Core
{
    public enum FileRepair { None, Build, Plugins, Shaders, Setup }

    public sealed class FileProblem
    {
        public string Area, What, Path;     // Path: the file, relative to the install where it is inside it ("" none)
        public FileRepair Repair;
        public bool Remove;                 // a broken file of the installer's own: removed before the repair makes it again
        public override string ToString() => Area + ": " + What;
    }

    public static class FileCheck
    {
        public const string ListName = "files_check.tsv";
        static readonly string[] Exefs = { "main", "main.npdm", "rtld", "sdk", "subsdk0" };
        /// <summary>the folders of dump\ whose files are listed with their sizes (their own files, not their folders')</summary>
        static readonly string[] Listed = { "exefs", "nro", "fonts", "modules" };

        public static bool Shown => Environment.GetEnvironmentVariable("OPENSMASH_FILE_CHECK") != "0";
        public static string ListPath(string root) => Path.Combine(root, "dump", ListName);
        public static bool HasList(string root) => File.Exists(ListPath(root));

        static long Size(string path) { try { return new FileInfo(path).Length; } catch (IOException) { return -1; } catch (UnauthorizedAccessException) { return -1; } }

        static IEnumerable<string> ListedFiles(string root)
        {
            foreach (string folder in Listed)
            {
                string dir = Path.Combine(root, "dump", folder);
                if (!Directory.Exists(dir)) continue;
                foreach (string f in Directory.EnumerateFiles(dir).OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
                    if (!f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) yield return f;
            }
        }

        /// <summary>the list written from the files as they are now (after a build that worked, or at the first check);
        /// the number of files listed</summary>
        public static int WriteList(string root)
        {
            var lines = new List<string>();
            foreach (string f in ListedFiles(root))
            {
                long n = Size(f);
                if (n > 0) lines.Add(n + "\t" + Path.GetRelativePath(root, f).Replace('\\', '/'));
            }
            Directory.CreateDirectory(Path.Combine(root, "dump"));
            string tmp = ListPath(root) + ".tmp";
            File.WriteAllLines(tmp, lines);
            File.Move(tmp, ListPath(root), true);
            return lines.Count;
        }

        static Dictionary<string, long> ReadList(string root)
        {
            var d = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
            try
            {
                foreach (string line in File.ReadAllLines(ListPath(root)))
                {
                    int tab = line.IndexOf('\t');
                    if (tab > 0 && long.TryParse(line.Substring(0, tab), out long n) && line.Length > tab + 1) d[line.Substring(tab + 1)] = n;
                }
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            return d;
        }

        static bool Inside(string root, string path)
        {
            string a = Path.GetFullPath(root).TrimEnd('\\', '/') + Path.DirectorySeparatorChar, b = Path.GetFullPath(path);
            return b.StartsWith(a, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>the install in root checked: its problems (none: whole). Nothing is written or removed.</summary>
        public static List<FileProblem> Check(string root)
        {
            var bad = new List<FileProblem>();
            void Add(string area, string what, FileRepair repair, string path = "", bool remove = false) =>
                bad.Add(new FileProblem { Area = area, What = what, Repair = repair, Path = path, Remove = remove });
            string dump = Path.Combine(root, "dump"), build = Path.Combine(root, "build");

            // the set-up's record, and whether the game files are still where the set-up read them (a repair reads them again)
            string folders = Path.Combine(dump, "build_folders.txt");
            string game = "";
            if (!File.Exists(folders)) Add("Set-up", "this install does not say where its game files were (dump\\build_folders.txt): run the set-up", FileRepair.Setup);
            else
                try
                {
                    foreach (string line in File.ReadAllLines(folders))
                        if (line.StartsWith("game=", StringComparison.OrdinalIgnoreCase)) game = line.Substring(5).Trim();
                }
                catch (IOException) { }
            bool gameThere = game != "" && (Directory.Exists(game) || File.Exists(game));

            // the program, the fighters, the fonts
            foreach (string n in Exefs)
                if (Size(Path.Combine(dump, "exefs", n)) <= 0) Add("Game program", "dump\\exefs\\" + n + " is missing", FileRepair.Build, "dump/exefs/" + n);
            string nro = Path.Combine(dump, "nro");
            if (!Directory.Exists(nro) || !Directory.EnumerateFiles(nro, "*.nro").Any()) Add("Fighters", "dump\\nro holds no fighter", FileRepair.Build);
            string fonts = Path.Combine(dump, "fonts");
            if (!File.Exists(fonts + ".none") && (!Directory.Exists(fonts) || !Directory.EnumerateFiles(fonts).Any()))
                Add("Fonts", "dump\\fonts is empty", FileRepair.Build);

            // the game data: romfs.cfg and each file it reads
            string cfg = Path.Combine(dump, "modules", "romfs.cfg");
            if (Size(cfg) <= 0) Add("Game data", "dump\\modules\\romfs.cfg is missing", FileRepair.Build, "dump/modules/romfs.cfg");
            else
                try
                {
                    foreach (string raw in File.ReadAllLines(cfg))
                    {
                        string line = raw.Trim();
                        if (line.Length < 3 || !Path.IsPathRooted(line)) continue;   // (a word such as "layered", an offset)
                        if (Size(line) > 0) continue;
                        if (Inside(root, line)) Add("Game data", Path.GetRelativePath(root, line) + " is missing", FileRepair.Build, Path.GetRelativePath(root, line).Replace('\\', '/'));
                        else Add("Game data", "the game's own file is not where it was: " + line + " (the data is read from it in place)", FileRepair.Setup);
                    }
                }
                catch (IOException) { }

            // the list of the installer's files with their sizes
            if (HasList(root))
            {
                DateTime listed = File.GetLastWriteTimeUtc(ListPath(root));
                foreach (var kv in ReadList(root))
                {
                    string path = Path.Combine(root, kv.Key.Replace('/', Path.DirectorySeparatorChar));
                    if (bad.Any(b => string.Equals(b.Path, kv.Key, StringComparison.OrdinalIgnoreCase))) continue;   // (said above)
                    long n = Size(path);
                    string area = kv.Key.StartsWith("dump/exefs/") ? "Game program" : kv.Key.StartsWith("dump/nro/") ? "Fighters"
                                : kv.Key.StartsWith("dump/fonts/") ? "Fonts" : "Game data";
                    if (n < 0) Add(area, kv.Key.Replace('/', '\\') + " is missing", FileRepair.Build, kv.Key);
                    else if (n == 0) Add(area, kv.Key.Replace('/', '\\') + " is empty", FileRepair.Build, kv.Key, true);
                    else if (n != kv.Value && File.GetLastWriteTimeUtc(path) <= listed)   // (newer than the list: a build made it again)
                        Add(area, kv.Key.Replace('/', '\\') + " is " + n + " bytes, the installer made " + kv.Value, FileRepair.Build, kv.Key, true);
                }
            }

            // the build
            if (Size(Path.Combine(build, "ssbu.exe")) <= 0) Add("Build", "build\\ssbu.exe is missing: the game is not built", FileRepair.Build);
            else
            {
                if (OperatingSystem.IsWindows() && Size(Path.Combine(build, "ssbu_glslang.dll")) <= 0)
                    Add("Build", "build\\ssbu_glslang.dll is missing (the Vulkan renderer's shader translator: Direct3D 11 draws instead)", FileRepair.Build);
                // the plugins built in: each has its image beside the game's modules
                string plugins = Path.Combine(dump, "skyline", "plugins.txt");
                if (File.Exists(plugins))
                    try
                    {
                        foreach (string raw in File.ReadAllLines(plugins))
                        {
                            string name = raw.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
                            if (name == "" || name.StartsWith("#")) continue;
                            if (Size(Path.Combine(dump, "modules", name + ".reloc.bin")) <= 0)
                                Add("Plugins", "the plugin " + name + " is listed but not built in", FileRepair.Plugins);
                        }
                    }
                    catch (IOException) { }
                // the shaders
                bool prepared = false;
                try
                {
                    string done = Path.Combine(dump, "shadercache", "complete.txt");
                    prepared = File.Exists(done) && File.ReadAllLines(done).Any(l => l.StartsWith("spirv ") || l.StartsWith("dxbc ") || l.Trim() == "complete");
                }
                catch (IOException) { }
                if (!prepared) Add("Shaders", "the shaders are not prepared yet (the game makes each at its first use, or prepares them at its start)", FileRepair.Shaders);
            }

            if (bad.Any(b => b.Repair == FileRepair.Build) && !gameThere && File.Exists(folders))
                Add("Set-up", "the game files are not where the set-up read them (" + (game == "" ? "no folder recorded" : game) + "): the repair will ask for them", FileRepair.Setup);
            return bad;
        }

        /// <summary>before a repair: the broken files of the installer's own removed (only inside dump\), so the
        /// installer's build step makes them again; the number removed</summary>
        public static int RemoveBroken(string root, IEnumerable<FileProblem> problems)
        {
            int n = 0;
            string dump = Path.Combine(root, "dump");
            foreach (var p in problems.Where(p => p.Remove && !string.IsNullOrEmpty(p.Path)))
            {
                string path = Path.Combine(root, p.Path.Replace('/', Path.DirectorySeparatorChar));
                if (!Inside(dump, path)) continue;
                try { if (File.Exists(path)) { File.Delete(path); n++; } }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
            return n;
        }

        /// <summary>what the repairs come to, in the order they are done: Setup first (nothing else can work without the
        /// game files), then Build, Plugins, Shaders</summary>
        public static FileRepair[] Repairs(IEnumerable<FileProblem> problems) =>
            new[] { FileRepair.Setup, FileRepair.Build, FileRepair.Plugins, FileRepair.Shaders }.Where(r => problems.Any(p => p.Repair == r)).ToArray();

        /// <summary>the report shown: one line a problem (at most max), or that the install is whole</summary>
        public static string Report(IReadOnlyList<FileProblem> problems, int max = 10)
        {
            if (problems.Count == 0) return "The install is whole: the game's program, data, fighters, build, plugins and shaders are all there.";
            var lines = problems.Take(max).Select(p => "- " + p).ToList();
            if (problems.Count > max) lines.Add("- ... and " + (problems.Count - max) + " more");
            return string.Join("\n", lines);
        }
    }
}
