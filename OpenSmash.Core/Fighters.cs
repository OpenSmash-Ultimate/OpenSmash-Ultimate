// SPDX-License-Identifier: GPL-3.0-or-later
// Fighters by any spelling, and their stock icons. Mastery points name a fighter by its game key ("jack", "pzenigame");
// SSBU Arena's ranked board by its display name ("Joker", "Pyra / Mythra"); people by nicknames ("PT", "ics"). Fighters.Key
// turns any of them into the game key, as opensmash-site's js/fighters.js does (the same alias table).
// Icons: Max_Hamm's stock icons as SSBU Arena serves them, https://www.ssbuarena.com/fighters/<key>/1.png (PNG, 200 px; the
// site's own pages use the .webp next to it, which System.Drawing cannot read). Nothing is kept in this repository; a key
// without an icon, or a download that fails, is drawn as the name's initial by the launchers. OPENSMASH_FIGHTER_ICONS
// names another base address (tests, a mirror).
// Each icon downloaded is kept on disk, %LOCALAPPDATA%\OpenSmash\fighter-icons\<folder>.png (~/.local/share/OpenSmash on
// Linux; OPENSMASH_SETTINGS_DIR moves the OpenSmash folder, as for project_folder.txt), and used from there for 30 days:
// every launcher start downloaded them all again, and they are most of what ssbuarena.com serves (its host counts each
// request). A folder that cannot be read or written is the memory alone, as before. OPENSMASH_ICON_CACHE=0: memory only.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public static class Fighters
    {
        static string Squash(string v)
        {
            var sb = new StringBuilder();
            foreach (char c in (v ?? "").ToLowerInvariant()) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) sb.Append(c);
            return sb.ToString();
        }

        static readonly Dictionary<string, string> Alias = BuildAlias();

        static Dictionary<string, string> BuildAlias()
        {
            var a = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var k in Mastery.Names.Keys) a[Squash(k)] = k;
            foreach (var kv in Mastery.Names) if (!a.ContainsKey(Squash(kv.Value))) a[Squash(kv.Value)] = kv.Key;
            const string pairs =
                "joker:jack hero:brave banjo:buddy banjokazooie:buddy terry:dolly terrybogard:dolly byleth:master minmin:tantan steve:pickel " +
                "alex:pickel sephiroth:edge pyra:eflame mythra:elight pyramythra:element aegis:element kazuya:demon sora:trail " +
                "pt:ptrainer pokemontrainer:ptrainer pokmontrainer:ptrainer trainer:ptrainer squirtle:pzenigame ivysaur:pfushigisou charizard:plizardon " +
                "iceclimbers:ice_climber iceclimber:ice_climber ics:ice_climber icies:ice_climber " +
                "mii:miifighter miifighter:miifighter miifighters:miifighter brawler:miifighter miibrawler:miifighter " +
                "miisword:miiswordsman miiswordfighter:miiswordsman swordfighter:miiswordsman miigunner:miigunner gunner:miigunner " +
                "dk:donkey donkeykong:donkey diddykong:diddy kingkrool:krool krool:krool dedede:dedede kingdedede:dedede " +
                "falcon:captain captainfalcon:captain cf:captain jigglypuff:purin puff:purin bowser:koopa bowserjr:koopajr " +
                "drmario:mariod doc:mariod younglink:younglink yink:younglink toonlink:toonlink tink:toonlink ganondorf:ganon ganon:ganon " +
                "gameandwatch:gamewatch mrgameandwatch:gamewatch gnw:gamewatch gw:gamewatch metaknight:metaknight mk:metaknight " +
                "darkpit:pitb darksamus:samusd zss:szerosuit zerosuitsamus:szerosuit olimar:pikmin alph:pikmin rob:robot " +
                "villager:murabito megaman:rockman wiifittrainer:wiifit wft:wiifit rosalina:rosetta rosalinaluma:rosetta luma:rosetta " +
                "littlemac:littlemac mac:littlemac greninja:gekkouga pacman:pacman robin:reflet corrin:kamui isabelle:shizue " +
                "incineroar:gaogaen piranhaplant:packun plant:packun duckhunt:duckhunt dh:duckhunt mrgamewatch:gamewatch belmont:simon";
            foreach (var p in pairs.Split(' '))
            {
                int c = p.IndexOf(':');
                if (c > 0) a[Squash(p.Substring(0, c))] = p.Substring(c + 1);
            }
            return a;
        }

        /// <summary>the game key of a fighter given any spelling (a key, a "ui_chara_" id, a name, a nickname); null when unknown</summary>
        public static string Key(string any)
        {
            string s = (any ?? "").Trim();
            if (s.StartsWith("ui_chara_", StringComparison.OrdinalIgnoreCase)) s = s.Substring(9);
            string q = Squash(s);
            return q != "" && Alias.TryGetValue(q, out var k) ? k : null;
        }

        /// <summary>a fighter's display name from any spelling; an unknown one as given</summary>
        public static string Name(string any) => Key(any) is { } k ? Mastery.FighterName(k) : (any ?? "").Trim();

        // the folders SSBU Arena serves (its src/lib/fighter-icons.ts); every one has 1.png
        static readonly HashSet<string> IconFolders = new HashSet<string>((
            "bayonetta brave buddy captain chrom cloud daisy dedede demon diddy dolly donkey duckhunt edge eflame elight " +
            "falco fox gamewatch ganon gaogaen gekkouga ice_climber ike inkling jack kamui ken kirby koopa koopajr krool link littlemac " +
            "lucario lucas lucina luigi mario mariod marth master metaknight mewtwo miifighter miigunner miiswordsman murabito ness " +
            "packun pacman palutena peach pfushigisou pichu pickel pikachu pikmin pit pitb plizardon ptrainer purin pzenigame reflet " +
            "richter ridley robot rockman rosetta roy ryu samus samusd sheik shizue shulk simon snake sonic szerosuit tantan toonlink " +
            "trail wario wiifit wolf yoshi younglink zelda").Split(' '));
        static readonly Dictionary<string, string> IconFolderOf = new Dictionary<string, string>
        {
            ["popo"] = "ice_climber", ["nana"] = "ice_climber", ["ptrainer_low"] = "ptrainer", ["element"] = "eflame",
            ["eflame_first"] = "eflame", ["eflame_only"] = "eflame", ["elight_first"] = "elight", ["elight_only"] = "elight",
        };

        public const string IconBase = "https://www.ssbuarena.com/fighters/";

        static string Base()
        {
            string b = Environment.GetEnvironmentVariable("OPENSMASH_FIGHTER_ICONS")?.Trim();
            return string.IsNullOrEmpty(b) ? IconBase : b.TrimEnd('/') + "/";
        }

        /// <summary>the icon folder of a fighter (any spelling), or null when SSBU Arena has none for it</summary>
        public static string IconFolder(string any)
        {
            string k = Key(any);
            if (k == null) return null;
            if (IconFolderOf.TryGetValue(k, out var f)) k = f;
            return IconFolders.Contains(k) ? k : null;
        }

        /// <summary>the icon's address (PNG), or null</summary>
        public static string IconUrl(string any) => IconFolder(any) is { } f ? Base() + f + "/1.png" : null;

        // ---- the downloaded icons, kept for the launcher's life (a failed one is not asked again) and on disk -----------
        static readonly ConcurrentDictionary<string, byte[]> Got = new ConcurrentDictionary<string, byte[]>();
        static readonly ConcurrentDictionary<string, byte> Asked = new ConcurrentDictionary<string, byte>();
        static readonly SemaphoreSlim Gate = new SemaphoreSlim(4);

        public const string CacheVariable = "OPENSMASH_ICON_CACHE";
        public static readonly TimeSpan CacheAge = TimeSpan.FromDays(30);
        const int MaxIcon = 512 * 1024;

        /// <summary>the folder the icons are kept in, or null (OPENSMASH_ICON_CACHE=0, or no per-user folder)</summary>
        public static string IconCacheDir()
        {
            string v = Environment.GetEnvironmentVariable(CacheVariable)?.Trim();
            if (v == "0" || string.Equals(v, "off", StringComparison.OrdinalIgnoreCase) || string.Equals(v, "false", StringComparison.OrdinalIgnoreCase)) return null;
            string dir = Environment.GetEnvironmentVariable("OPENSMASH_SETTINGS_DIR");
            if (string.IsNullOrEmpty(dir))
            {
                string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(local)) return null;
                dir = Path.Combine(local, "OpenSmash");
            }
            return Path.Combine(dir, "fighter-icons");
        }

        // (a PNG and a sane size: nothing else is handed to the image decoders)
        static bool IsPng(byte[] b) => b != null && b.Length >= 8 && b.Length <= MaxIcon && b[0] == 0x89 && b[1] == (byte)'P' && b[2] == (byte)'N' && b[3] == (byte)'G';

        /// <summary>the icon's PNG bytes when downloaded (or kept on disk); null meanwhile (the download starts, `changed` is
        /// called when it is there) or when there is none. Never throws.</summary>
        public static byte[] Icon(HttpClient http, string any, Action changed = null)
        {
            string folder = IconFolder(any);
            if (folder == null || http == null) return null;
            string url = Base() + folder + "/1.png";
            if (Got.TryGetValue(url, out var bytes)) return bytes;
            // (on the thread pool: the disk is read there too, and the callers are painting)
            if (Asked.TryAdd(url, 0)) _ = Task.Run(() => Fetch(http, url, folder, changed));
            return null;
        }

        static async Task Fetch(HttpClient http, string url, string folder, Action changed)
        {
            string file = CacheFile(folder);
            if (FromDisk(file) is { } kept)
            {
                Got[url] = kept;
                try { changed?.Invoke(); } catch { }
                return;
            }
            await Gate.WaitAsync().ConfigureAwait(false);
            try
            {
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
                using var r = await http.GetAsync(url, cts.Token).ConfigureAwait(false);
                if (!r.IsSuccessStatusCode) return;
                var b = await r.Content.ReadAsByteArrayAsync(cts.Token).ConfigureAwait(false);
                if (!IsPng(b)) return;
                Got[url] = b;
                ToDisk(file, b);
                try { changed?.Invoke(); } catch { }
            }
            catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is OperationCanceledException || e is InvalidOperationException) { }
            finally { Gate.Release(); }
        }

        /// <summary>the kept icon's file, &lt;cache&gt;/&lt;folder&gt;.png, or null (no cache; a folder not of letters, digits and "_")</summary>
        static string CacheFile(string folder)
        {
            if (string.IsNullOrEmpty(folder) || folder.Any(c => !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))) return null;
            try { return IconCacheDir() is { } dir ? Path.Combine(dir, folder + ".png") : null; }
            catch (Exception e) when (e is ArgumentException || e is PlatformNotSupportedException) { return null; }
        }

        /// <summary>the kept icon when it is a PNG younger than 30 days; a damaged one is removed (and downloaded again)</summary>
        static byte[] FromDisk(string file)
        {
            if (file == null) return null;
            try
            {
                var info = new FileInfo(file);
                if (!info.Exists || DateTime.UtcNow - info.LastWriteTimeUtc > CacheAge) return null;
                if (info.Length <= MaxIcon && File.ReadAllBytes(file) is var b && IsPng(b)) return b;
                File.Delete(file);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException || e is NotSupportedException || e is System.Security.SecurityException) { }
            return null;
        }

        /// <summary>the icon kept: written next to its file and moved over it, so a half-written one is never read. Any error
        /// (a folder that is missing and cannot be made, read-only, full): not kept, the memory alone.</summary>
        static void ToDisk(string file, byte[] b)
        {
            if (file == null) return;
            string tmp = null;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(file));
                tmp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
                File.WriteAllBytes(tmp, b);
                File.Move(tmp, file, true);
                tmp = null;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException || e is NotSupportedException || e is System.Security.SecurityException) { }
            finally
            {
                if (tmp != null) try { File.Delete(tmp); } catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
            }
        }

        /// <summary>tests: the cache emptied</summary>
        public static void ForgetIcons() { Got.Clear(); Asked.Clear(); }
    }
}
