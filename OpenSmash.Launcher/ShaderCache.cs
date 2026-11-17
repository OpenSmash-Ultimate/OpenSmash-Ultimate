using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace OpenSmash.Launcher
{
    // Settings > Storage > REBUILD SHADERS: what goes from this install's dump\shadercache before Prepare Shaders runs again.
    // tools/shader_aot.py compiles only the keys not in the cache yet, so a rebuild removes the translated shaders too.
    // Everything is named by the game's own patterns (runtime/gpu_shader_cache.h, gpu_vk.c, gpu_d3d11.c, gpu_nvcache.c);
    // a file the list does not know (drawn_keys.txt: the pairings the game found at a first draw, which shader_aot.py
    // reads back) stays. Only the top level of the cache folder and its nvidia\ folder; never anything outside it.
    internal static class ShaderCache
    {
        static readonly Regex[] Compiled =
        {
            new(@"^v\d+_(vs|ps|cs)_[0-9a-f]+_[0-9a-f]+_[0-9a-f]+\.dxbc$", RegexOptions.IgnoreCase),    // D3D11 DXBC
            new(@"^g\d+([._][0-9a-f]+)?_(vs|ps|cs)_[0-9a-f]+_[0-9a-f]+_[0-9a-f]+\.spv$", RegexOptions.IgnoreCase),   // Vulkan SPIR-V (old names: g1001_...)
            new(@"^m\d+_(vs|ps|cs)_[0-9a-f]+_[0-9a-f]+_[0-9a-f]+\.metal$", RegexOptions.IgnoreCase),  // macOS MSL
            new(@"^vulkan_[0-9a-f]{4}_[0-9a-f]{4}\.pipelines$", RegexOptions.IgnoreCase),           // Vulkan pipeline cache
            new(@"^vulkan_[0-9a-f]{4}_[0-9a-f]{4}\.warm(\.warming|\.capped)?$", RegexOptions.IgnoreCase),   // warm list, markers (#430 cap)
            new(@"\.tmp$", RegexOptions.IgnoreCase),                                                   // writes cut short
        };

        static readonly string[] Markers =
        {
            "complete.txt",        // Prepare Shaders finished (the launcher's ShadersDone)
            "prepare_tries.txt",   // the game's own preparation attempts
            "warm.done", "prewarm-all.done", "warm.log", "warm.lock",   // D3D11 warm passes
        };

        public static string Folder(string repo) => Path.Combine(repo, "dump", "shadercache");

        // The files and folders a rebuild removes, complete.txt first (so a rebuild cut short still reads as not done).
        public static List<string> RebuildTargets(string repo)
        {
            var list = new List<string>();
            string dir = Folder(repo);
            if (!Directory.Exists(dir)) return list;
            string marker = Path.Combine(dir, "complete.txt");
            if (File.Exists(marker)) list.Add(marker);
            foreach (string path in Directory.EnumerateFiles(dir, "*", SearchOption.TopDirectoryOnly))
            {
                string name = Path.GetFileName(path);
                if (name.Equals("complete.txt", StringComparison.OrdinalIgnoreCase)) continue;
                if (Array.Exists(Markers, m => m.Equals(name, StringComparison.OrdinalIgnoreCase)) || Array.Exists(Compiled, r => r.IsMatch(name)))
                    list.Add(path);
            }
            string nvidia = Path.Combine(dir, "nvidia");
            // (a link there would lead outside the install: left alone)
            if (Directory.Exists(nvidia) && !new DirectoryInfo(nvidia).Attributes.HasFlag(FileAttributes.ReparsePoint)) list.Add(nvidia);
            return list;
        }

        // Removes them; the paths that could not go (a file a running game holds open), with why.
        public static List<string> Clear(string repo)
        {
            var failed = new List<string>();
            foreach (string path in RebuildTargets(repo))
            {
                try
                {
                    if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
                    else File.Delete(path);
                }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
                {
                    failed.Add(Path.GetFileName(path) + ": " + e.Message);
                }
            }
            return failed;
        }
    }
}
