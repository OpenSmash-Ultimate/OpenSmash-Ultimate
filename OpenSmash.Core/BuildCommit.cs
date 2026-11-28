// SPDX-License-Identifier: GPL-3.0-or-later
// The commit build\ssbu.exe was built from (SSBU_BUILD_COMMIT_CHECK, on unless 0): the build writes it into the program
// right after linking it (tools/build_commit.py, runtime/hle_build_commit.c) and records it in build\build_commit.txt
// with where it is in the file. Before, the launcher named tools/source_version.txt's commit (the sources', which the
// updater writes whether the game was built again after it or not) in crash reports: two PCs ran a build of 17/11 and
// reported v0.5.4's commit. Now a report names the build's commit, the UPDATE card shows it when it is not the sources'
// one, and a build that is behind its sources (an update whose build was skipped or failed) makes Update build it.
// 0: the sources' commit everywhere and no check, as before.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace OpenSmash.Core
{
    public static class BuildCommit
    {
        public const string Variable = "SSBU_BUILD_COMMIT_CHECK";
        public static bool On => (Environment.GetEnvironmentVariable(Variable) ?? "").Trim() != "0";
        static readonly byte[] Marker = Encoding.ASCII.GetBytes("SSBU-BUILD-COMMIT:");
        const int Size = 40;

        public static string Program(string root) => Path.Combine(root, "build", "ssbu.exe");
        public static string RecordFile(string root) => Path.Combine(root, "build", "build_commit.txt");

        static bool Commit(string t) => t != null && t.Length == Size && t.All(c => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));

        /// <summary>build\build_commit.txt: its keys (commit, inputs, exe_at, library, library_at) and the commits it is
        /// current for ("current"), or null when there is none</summary>
        public static Dictionary<string, string> Load(string root)
        {
            try
            {
                string f = RecordFile(root);
                if (!File.Exists(f)) return null;
                var d = new Dictionary<string, string>(StringComparer.Ordinal);
                foreach (string raw in File.ReadAllLines(f))
                {
                    string line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#') continue;
                    int sp = line.IndexOf(' ');
                    string key = sp < 0 ? line : line.Substring(0, sp), value = sp < 0 ? "" : line.Substring(sp + 1).Trim();
                    d[key] = key == "current" && d.TryGetValue("current", out var was) ? was + " " + value : value;
                }
                return d;
            }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }

        /// <summary>the marker's 40 bytes at the offset given, or null when the marker is not there</summary>
        static string At(FileStream fs, long at)
        {
            if (at < Marker.Length || at + Size > fs.Length) return null;
            var b = new byte[Marker.Length + Size];
            fs.Seek(at - Marker.Length, SeekOrigin.Begin);
            int n = 0, r;
            while (n < b.Length && (r = fs.Read(b, n, b.Length - n)) > 0) n += r;
            if (n < b.Length || !b.Take(Marker.Length).SequenceEqual(Marker)) return null;
            return Encoding.ASCII.GetString(b, Marker.Length, Size);
        }

        /// <summary>the first marker's 40 bytes in the file, read through (a build without a record), or null</summary>
        static string Scan(FileStream fs)
        {
            const int chunk = 4 << 20;
            var buf = new byte[chunk + Marker.Length + Size];
            int keep = 0;
            fs.Seek(0, SeekOrigin.Begin);
            while (true)
            {
                int n = fs.Read(buf, keep, chunk);
                if (n <= 0) return null;
                int len = keep + n;
                for (int i = 0; i + Marker.Length + Size <= len; i++)
                {
                    if (buf[i] != Marker[0]) continue;
                    int j = 1;
                    while (j < Marker.Length && buf[i + j] == Marker[j]) j++;
                    if (j == Marker.Length)
                    {
                        string v = Encoding.ASCII.GetString(buf, i + Marker.Length, Size);
                        if (Commit(v) || v == new string('?', Size)) return v;
                    }
                }
                keep = Math.Min(len, Marker.Length + Size - 1);
                Buffer.BlockCopy(buf, len - keep, buf, 0, keep);
            }
        }

        /// <summary>what build\ssbu.exe says it was built from: its commit; "" when it has the place for it but the build
        /// did not write it; null when it has no place for it (a build of before v0.5.5), is not there or cannot be read.
        /// scan: look through the whole file when the record does not say where (slow for the 1 GB single-program builds:
        /// never on the UI thread).</summary>
        public static string Read(string root, bool scan = true)
        {
            try
            {
                string exe = Program(root);
                if (!File.Exists(exe)) return null;
                var rec = Load(root);
                using var fs = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                string v = null;
                if (rec != null && rec.TryGetValue("exe_at", out var a) && long.TryParse(a, out long at)) v = At(fs, at);
                if (v == null && scan) v = Scan(fs);
                if (v == null) return null;
                return Commit(v) ? v : "";
            }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }

        /// <summary>the commit a report names: the build's when it says it, else the sources' (tools/source_version.txt);
        /// with SSBU_BUILD_COMMIT_CHECK=0 the sources', as before</summary>
        public static string Reported(string root)
        {
            if (On)
            {
                string built = Read(root, scan: false);
                if (Commit(built)) return built;
            }
            return Updater.InstalledCommit(root);
        }

        /// <summary>the build is not of the sources' commit and was not found current for it (tools/build_commit.py reason:
        /// an update of only what is not built, the launcher's): an update's build was skipped or failed, and Update builds
        /// it. A build of before v0.5.5 (no place for the commit) is behind too; one whose commit was never written is not
        /// (the files' dates decide, tools/skyline.py uptodate). Windows only: there the build writes it.</summary>
        public static bool Behind(string root, string sources)
        {
            if (!On || !OperatingSystem.IsWindows() || !Commit(sources) || !File.Exists(Program(root))) return false;
            string built = Read(root);
            if (built == null) return true;
            if (built.Length == 0 || built == sources) return false;
            var rec = Load(root);
            return rec == null || !rec.TryGetValue("current", out var current) || !current.Split(' ').Contains(sources);
        }

        /// <summary>what the UPDATE card adds after the sources' commit: " · BUILD abc1234" when the build says another
        /// commit; "" otherwise (read only where the record says: no reading through the program while drawing)</summary>
        public static string Shown(string root)
        {
            if (!On || string.IsNullOrEmpty(root)) return "";
            string built = Read(root, scan: false), sources = Updater.InstalledCommit(root);
            return Commit(built) && sources != null && built != sources ? "   ·   BUILD " + built.Substring(0, 7) : "";
        }
    }
}
