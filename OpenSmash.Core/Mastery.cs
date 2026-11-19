// SPDX-License-Identifier: GPL-3.0-or-later
// Mastery points (they replace the GSP views): per account and per fighter, points that only go up. No levels, no tiers,
// no ranks of skill: points only (the competitive ladder is SSBU Arena's ranked). Each official lobby counts only the
// matches it hosted, so the launcher SUMS the regions' answers; the one-time veteran bonus (for a GSP record a lobby had)
// is kept apart and the largest of the regions' counts, not their sum:
//   a fighter's total = sum over regions of points + max over regions of veteran.
// The lobby's pages (plain HTTP, as /gsp/me):
//   GET /mastery/me    header X-OpenSmash-Ticket -> {"region","account","total","fighters":[{"fighter","points","veteran","games","wins","last_at"}]}
//   GET /mastery.json  public -> {"region","updated","fighters":{"<key>":[{"id","name","points","veteran"}...]},"total":[...]}
// A lobby without them (older) answers 404: nothing is shown ("not available"), the launcher never fails on it.
// The ticket goes to the official lobbies only (OfficialLobby.All), as LobbyBoard's.
// The accounts service (Supabase, the one source of mastery points: the lobbies push their counts to it every hour) is
// asked FIRST, with the account's own session (Account, OPENSMASH_SUPABASE_URL / _KEY as for the sign-in):
//   POST /rest/v1/rpc/my_opensmash_mastery()        signed in -> [{fighter, points, veteran, total, games, wins, updated_at,
//                                                    points_opensmash, points_arena}]
//   POST /rest/v1/rpc/my_opensmash_mastery_rank()   signed in -> [{pos, id, total, total_count, main_fighter}]
//   POST /rest/v1/rpc/opensmash_mastery_board(p_fighter, p_limit, p_offset)   anyone (publishable key) ->
//        [{pos, id, name (null: "Anonymous"), tag, avatar, main_fighter, total, games, total_count}]
// The lobbies' pages are only the fallback when the service cannot be reached or does not have the functions yet.
// Switches: OPENSMASH_MASTERY_UI=0 brings back the GSP views; OPENSMASH_ARENA_CARD=0 hides the SSBU Arena card;
// OPENSMASH_TOP_MASTERY=0 hides the "Top mastery" list under it.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class MasteryFighter
    {
        public string Fighter = "";   // the ui_chara_db key without "ui_chara_" ("mario", "pzenigame", "miifighter")
        public long Points;           // summed over the regions (without the veteran bonus)
        public int Veteran;           // the largest of the regions'
        public int Games, Wins;
        public long? Given;           // the accounts service's own "total" (it may count more than points + veteran)
        public long Total => Given ?? Points + Veteran;
    }

    /// <summary>the account's place among all the accounts with mastery points (my_opensmash_mastery_rank())</summary>
    public sealed class MasteryRank
    {
        public long Pos, Total, TotalCount;
        public string Id = "", MainFighter = "";
    }

    public sealed class MasteryMe
    {
        public int Regions;   // how many lobbies answered (0: the accounts service's answer)
        public bool FromService;   // the accounts service (the source of mastery points), not a lobby's fallback
        public MasteryRank Rank;   // null: not known (a lobby's answer, or no place yet)
        public List<MasteryFighter> Fighters = new List<MasteryFighter>();   // highest total first
        public long Total => Fighters.Sum(f => f.Total);
        public int Games => Fighters.Sum(f => f.Games);
        public int Wins => Fighters.Sum(f => f.Wins);
    }

    public sealed class MasteryRow
    {
        public string Id = "", Name = "", MainFighter = "";
        public long Pos;   // the board's place (the accounts service's; 0 from a lobby)
        public int Tag;
        public long Points;
        public int Veteran;
        public long? Given;   // the accounts service's total
        public long Total => Given ?? Points + Veteran;
    }

    public static class Mastery
    {
        static bool Switch(string name)
        {
            string v = Environment.GetEnvironmentVariable(name)?.Trim();
            return !(v == "0" || string.Equals(v, "off", StringComparison.OrdinalIgnoreCase) || string.Equals(v, "false", StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>the mastery views (OPENSMASH_MASTERY_UI=0: the GSP views as before)</summary>
        public static bool UiOn => Switch("OPENSMASH_MASTERY_UI");
        /// <summary>the home's SSBU Arena card (OPENSMASH_ARENA_CARD=0 hides it)</summary>
        public static bool ArenaCardOn => Switch("OPENSMASH_ARENA_CARD");
        /// <summary>the "Top mastery" list under the card (OPENSMASH_TOP_MASTERY=0 hides it)</summary>
        public static bool TopOn => Switch("OPENSMASH_TOP_MASTERY");

        static long Long(JsonElement o, string name) =>
            o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out long n) ? Math.Max(0, n) : 0;

        static int Int(JsonElement o, string name) => (int)Math.Min(int.MaxValue, Long(o, name));

        static string Str(JsonElement o, string name) =>
            o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

        /// <summary>a fighter key as the lobby sends it, or "" when it is not one (lowercase letters, digits, '_', at most 32)</summary>
        public static string Key(string k)
        {
            k = (k ?? "").Trim().ToLowerInvariant();
            if (k.StartsWith("ui_chara_", StringComparison.Ordinal)) k = k.Substring(9);
            return k.Length > 0 && k.Length <= 32 && k.All(c => (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') ? k : "";
        }

        /// <summary>one lobby's /mastery/me; null when it is not one</summary>
        public static MasteryMe ParseMe(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                var r = d.RootElement;
                if (r.ValueKind != JsonValueKind.Object || !r.TryGetProperty("fighters", out var fs) || fs.ValueKind != JsonValueKind.Array) return null;
                var me = new MasteryMe { Regions = 1 };
                foreach (var f in fs.EnumerateArray())
                {
                    if (f.ValueKind != JsonValueKind.Object) continue;
                    string key = Key(Str(f, "fighter"));
                    if (key == "") continue;
                    me.Fighters.Add(new MasteryFighter
                    {
                        Fighter = key, Points = Long(f, "points"), Veteran = Math.Min(1000, Int(f, "veteran")),
                        Games = Int(f, "games"), Wins = Int(f, "wins"),
                    });
                }
                return Merge(new[] { me });
            }
            catch (JsonException) { return null; }
        }

        /// <summary>the regions' answers as one: points, games and wins summed, the largest veteran bonus; null answers skipped</summary>
        public static MasteryMe Merge(IEnumerable<MasteryMe> answers)
        {
            var all = new Dictionary<string, MasteryFighter>();
            int regions = 0;
            foreach (var me in answers)
            {
                if (me == null) continue;
                regions += Math.Max(1, me.Regions);
                foreach (var f in me.Fighters)
                {
                    if (!all.TryGetValue(f.Fighter, out var sum)) all[f.Fighter] = sum = new MasteryFighter { Fighter = f.Fighter };
                    sum.Points += f.Points; sum.Games += f.Games; sum.Wins += f.Wins;
                    sum.Veteran = Math.Max(sum.Veteran, f.Veteran);
                }
            }
            return new MasteryMe
            {
                Regions = regions,
                Fighters = all.Values.Where(f => f.Total > 0 || f.Games > 0).OrderByDescending(f => f.Total).ThenByDescending(f => f.Games).ThenBy(f => f.Fighter, StringComparer.Ordinal).ToList(),
            };
        }

        static async Task<string> Get(HttpClient http, string url, string ticket, CancellationToken cancel)
        {
            try
            {
                using var req = new HttpRequestMessage(HttpMethod.Get, url);
                if (!string.IsNullOrEmpty(ticket)) req.Headers.TryAddWithoutValidation(LobbyBoard.TicketHeader, ticket);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                timeout.CancelAfter(TimeSpan.FromSeconds(8));
                using var r = await http.SendAsync(req, timeout.Token);
                return r.IsSuccessStatusCode ? await r.Content.ReadAsStringAsync(timeout.Token) : null;   // (404: a lobby before mastery)
            }
            catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is OperationCanceledException || e is InvalidOperationException) { return null; }
        }

        /// <summary>GET /mastery/me with the ticket on each official lobby, summed; null when none answered with one (no
        /// ticket, every lobby older or down)</summary>
        public static async Task<MasteryMe> MeAsync(HttpClient http, IEnumerable<string> lobbies, string ticket, CancellationToken cancel)
        {
            if (string.IsNullOrEmpty(ticket)) return null;
            var got = await Task.WhenAll(lobbies.Distinct(StringComparer.OrdinalIgnoreCase)
                .Select(async lobby => { string t = await Get(http, $"http://{lobby}/mastery/me", ticket, cancel); return t == null ? null : ParseMe(t); }));
            return got.Any(m => m != null) ? Merge(got) : null;
        }

        /// <summary>one lobby's /mastery.json "total" list (the accounts by their total there); null when it is not one</summary>
        public static List<MasteryRow> ParseBoard(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                var r = d.RootElement;
                if (r.ValueKind != JsonValueKind.Object || !r.TryGetProperty("total", out var rows) || rows.ValueKind != JsonValueKind.Array) return null;
                var list = new List<MasteryRow>();
                foreach (var p in rows.EnumerateArray())
                {
                    if (p.ValueKind != JsonValueKind.Object) continue;
                    string id = Str(p, "id");
                    if (id == "" || id.Length > 64) continue;
                    list.Add(new MasteryRow { Id = id, Name = Clean(Str(p, "name")), Points = Long(p, "points"), Veteran = Int(p, "veteran") });
                }
                return list;
            }
            catch (JsonException) { return null; }
        }

        /// <summary>the regions' lists as one, by the opaque id (the same for an account on every lobby): points summed, the
        /// largest veteran (a "total" row's veteran is already that lobby's sum over the account's fighters); highest total first</summary>
        public static List<MasteryRow> MergeBoard(IEnumerable<List<MasteryRow>> boards, int take = 10)
        {
            var all = new Dictionary<string, MasteryRow>();
            foreach (var b in boards)
            {
                if (b == null) continue;
                foreach (var r in b)
                {
                    if (!all.TryGetValue(r.Id, out var sum)) all[r.Id] = sum = new MasteryRow { Id = r.Id };
                    sum.Points += r.Points;
                    sum.Veteran = Math.Max(sum.Veteran, r.Veteran);
                    if (sum.Name == "") sum.Name = r.Name;
                }
            }
            return all.Values.Where(r => r.Total > 0).OrderByDescending(r => r.Total).ThenBy(r => r.Id, StringComparer.Ordinal).Take(Math.Max(0, take)).ToList();
        }

        /// <summary>GET /mastery.json on each official lobby (public), merged; null when none answered with one</summary>
        public static async Task<List<MasteryRow>> BoardAsync(HttpClient http, IEnumerable<string> lobbies, CancellationToken cancel, int take = 10)
        {
            var got = await Task.WhenAll(lobbies.Distinct(StringComparer.OrdinalIgnoreCase)
                .Select(async lobby => { string t = await Get(http, $"http://{lobby}/mastery.json", null, cancel); return t == null ? null : ParseBoard(t); }));
            return got.Any(b => b != null) ? MergeBoard(got, take) : null;
        }

        // ---- the accounts service (the source; the lobbies above are the fallback) ------------------------------------

        static IEnumerable<JsonElement> Rows(JsonElement r) =>
            r.ValueKind == JsonValueKind.Array ? r.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.Object)
            : r.ValueKind == JsonValueKind.Object ? new[] { r } : Enumerable.Empty<JsonElement>();

        /// <summary>my_opensmash_mastery()'s rows as the account's mastery; null when the answer is not one</summary>
        public static MasteryMe ParseServiceMe(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                if (d.RootElement.ValueKind != JsonValueKind.Array) return null;
                var me = new MasteryMe { FromService = true };
                foreach (var f in Rows(d.RootElement))
                {
                    string key = Key(Str(f, "fighter"));
                    if (key == "") continue;
                    var row = new MasteryFighter
                    {
                        Fighter = key, Points = Long(f, "points"), Veteran = Math.Min(1000, Int(f, "veteran")),
                        Games = Int(f, "games"), Wins = Int(f, "wins"),
                    };
                    if (f.TryGetProperty("total", out var t) && t.ValueKind == JsonValueKind.Number) row.Given = Long(f, "total");
                    if (row.Total > 0 || row.Games > 0) me.Fighters.Add(row);
                }
                me.Fighters = me.Fighters.OrderByDescending(f => f.Total).ThenByDescending(f => f.Games).ThenBy(f => f.Fighter, StringComparer.Ordinal).ToList();
                return me;
            }
            catch (JsonException) { return null; }
        }

        /// <summary>my_opensmash_mastery_rank()'s row; null when there is none (no points yet) or the answer is not one</summary>
        public static MasteryRank ParseServiceRank(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                var r = Rows(d.RootElement).FirstOrDefault();
                if (r.ValueKind != JsonValueKind.Object || Long(r, "pos") <= 0) return null;
                return new MasteryRank { Pos = Long(r, "pos"), Total = Long(r, "total"), TotalCount = Long(r, "total_count"), Id = Str(r, "id"), MainFighter = Key(Str(r, "main_fighter")) };
            }
            catch (JsonException) { return null; }
        }

        /// <summary>opensmash_mastery_board()'s rows, in its order; null when the answer is not one</summary>
        public static List<MasteryRow> ParseServiceBoard(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                if (d.RootElement.ValueKind != JsonValueKind.Array) return null;
                var list = new List<MasteryRow>();
                foreach (var p in Rows(d.RootElement))
                {
                    string id = Str(p, "id");
                    if (id == "" || id.Length > 64) continue;
                    string name = Clean(Str(p, "name"));
                    list.Add(new MasteryRow
                    {
                        Id = id, Name = name == "" ? "Anonymous" : name, Pos = Long(p, "pos"), Tag = Int(p, "tag"),
                        MainFighter = Key(Str(p, "main_fighter")), Points = Long(p, "total"), Given = Long(p, "total"),
                    });
                }
                return list;
            }
            catch (JsonException) { return null; }
        }

        static bool ServiceUp(Account account) => account != null && account.Service.Configured;

        /// <summary>the signed-in account's mastery from the accounts service, with its place when it has one; null when
        /// not signed in or the service did not answer with it (then the lobbies are asked)</summary>
        public static async Task<MasteryMe> ServiceMeAsync(Account account)
        {
            if (!ServiceUp(account) || !account.SignedIn) return null;
            try
            {
                var me = ParseServiceMe(await account.RestAsync("POST", "/rest/v1/rpc/my_opensmash_mastery", "{}"));
                if (me == null) return null;
                try { me.Rank = ParseServiceRank(await account.RestAsync("POST", "/rest/v1/rpc/my_opensmash_mastery_rank", "{}")); }
                catch (Exception e) when (e is AccountException || e is HttpRequestException || e is TaskCanceledException) { }   // (the points without the place)
                return me;
            }
            catch (Exception e) when (e is AccountException || e is HttpRequestException || e is TaskCanceledException || e is InvalidOperationException) { return null; }
        }

        /// <summary>the top of the board from the accounts service (anyone may read it); null when it did not answer</summary>
        public static async Task<List<MasteryRow>> ServiceBoardAsync(Account account, int take = 5, string fighter = null)
        {
            if (!ServiceUp(account)) return null;
            try
            {
                string body = JsonSerializer.Serialize(new Dictionary<string, object> { ["p_fighter"] = fighter, ["p_limit"] = Math.Max(1, take), ["p_offset"] = 0 });
                var rows = ParseServiceBoard(await account.AnonRpcAsync("opensmash_mastery_board", body));
                return rows?.Take(Math.Max(0, take)).ToList();
            }
            catch (Exception e) when (e is AccountException || e is HttpRequestException || e is TaskCanceledException || e is InvalidOperationException) { return null; }
        }

        /// <summary>the account's mastery: the accounts service, else (it failed) the official lobbies with the ticket</summary>
        public static async Task<MasteryMe> MineAsync(Account account, HttpClient http, IEnumerable<string> lobbies, string ticket, CancellationToken cancel)
            => await ServiceMeAsync(account) ?? await MeAsync(http, lobbies, ticket, cancel);

        /// <summary>the top mastery: the accounts service, else (it failed) the official lobbies' lists merged</summary>
        public static async Task<List<MasteryRow>> TopAsync(Account account, HttpClient http, IEnumerable<string> lobbies, CancellationToken cancel, int take = 5)
            => await ServiceBoardAsync(account, take) ?? await BoardAsync(http, lobbies, cancel, take);

        /// <summary>the place as the launchers show it ("#12"), "" when there is none</summary>
        public static string Place(MasteryRank r) => r != null && r.Pos > 0 ? "#" + r.Pos.ToString(CultureInfo.InvariantCulture) : "";

        /// <summary>a shown name as one short line (the lobby limits it already)</summary>
        public static string Clean(string name)
        {
            var sb = new StringBuilder();
            foreach (char c in name ?? "") if (!char.IsControl(c)) sb.Append(c);
            string s = sb.ToString().Trim();
            return s.Length > 32 ? s.Substring(0, 32) : s;
        }

        /// <summary>what a row of the top list shows: the chosen name, else "Player"</summary>
        public static string Who(MasteryRow r) => r.Name != "" ? r.Name : "Player";

        public static string N(long v) => v.ToString("N0", CultureInfo.InvariantCulture);

        // the fighter keys' names (the same list as tools/modmanager/describe.py)
        internal static readonly Dictionary<string, string> Names = new Dictionary<string, string>
        {
            ["mario"] = "Mario", ["donkey"] = "Donkey Kong", ["link"] = "Link", ["samus"] = "Samus", ["samusd"] = "Dark Samus",
            ["yoshi"] = "Yoshi", ["kirby"] = "Kirby", ["fox"] = "Fox", ["pikachu"] = "Pikachu", ["luigi"] = "Luigi", ["ness"] = "Ness",
            ["captain"] = "Captain Falcon", ["purin"] = "Jigglypuff", ["peach"] = "Peach", ["daisy"] = "Daisy", ["koopa"] = "Bowser",
            ["ice_climber"] = "Ice Climbers", ["popo"] = "Ice Climbers", ["nana"] = "Ice Climbers", ["sheik"] = "Sheik", ["zelda"] = "Zelda",
            ["mariod"] = "Dr. Mario", ["pichu"] = "Pichu", ["falco"] = "Falco", ["marth"] = "Marth", ["lucina"] = "Lucina",
            ["younglink"] = "Young Link", ["ganon"] = "Ganondorf", ["mewtwo"] = "Mewtwo", ["roy"] = "Roy", ["chrom"] = "Chrom",
            ["gamewatch"] = "Mr. Game & Watch", ["metaknight"] = "Meta Knight", ["pit"] = "Pit", ["pitb"] = "Dark Pit",
            ["szerosuit"] = "Zero Suit Samus", ["wario"] = "Wario", ["snake"] = "Snake", ["ike"] = "Ike", ["ptrainer"] = "Pokemon Trainer",
            ["pzenigame"] = "Squirtle", ["pfushigisou"] = "Ivysaur", ["plizardon"] = "Charizard", ["diddy"] = "Diddy Kong",
            ["lucas"] = "Lucas", ["sonic"] = "Sonic", ["dedede"] = "King Dedede", ["pikmin"] = "Olimar", ["lucario"] = "Lucario",
            ["robot"] = "R.O.B.", ["toonlink"] = "Toon Link", ["wolf"] = "Wolf", ["murabito"] = "Villager", ["rockman"] = "Mega Man",
            ["wiifit"] = "Wii Fit Trainer", ["rosetta"] = "Rosalina & Luma", ["littlemac"] = "Little Mac", ["gekkouga"] = "Greninja",
            ["palutena"] = "Palutena", ["pacman"] = "Pac-Man", ["reflet"] = "Robin", ["shulk"] = "Shulk", ["koopajr"] = "Bowser Jr.",
            ["duckhunt"] = "Duck Hunt", ["ryu"] = "Ryu", ["ken"] = "Ken", ["cloud"] = "Cloud", ["kamui"] = "Corrin", ["bayonetta"] = "Bayonetta",
            ["inkling"] = "Inkling", ["ridley"] = "Ridley", ["simon"] = "Simon", ["richter"] = "Richter", ["krool"] = "King K. Rool",
            ["shizue"] = "Isabelle", ["gaogaen"] = "Incineroar", ["packun"] = "Piranha Plant", ["jack"] = "Joker", ["brave"] = "Hero",
            ["buddy"] = "Banjo & Kazooie", ["dolly"] = "Terry", ["master"] = "Byleth", ["tantan"] = "Min Min", ["pickel"] = "Steve",
            ["edge"] = "Sephiroth", ["eflame"] = "Pyra", ["elight"] = "Mythra", ["element"] = "Pyra & Mythra",
            ["eflame_first"] = "Pyra", ["elight_first"] = "Mythra", ["eflame_only"] = "Pyra", ["elight_only"] = "Mythra",
            ["demon"] = "Kazuya", ["trail"] = "Sora",
            ["miifighter"] = "Mii Brawler", ["miiswordsman"] = "Mii Swordfighter", ["miigunner"] = "Mii Gunner",
        };

        /// <summary>a fighter key's name ("pzenigame" -> "Squirtle"); an unknown key as itself, capitalized</summary>
        public static string FighterName(string key)
        {
            key = Key(key);
            if (key == "") return "?";
            return Names.TryGetValue(key, out var n) ? n : char.ToUpperInvariant(key[0]) + key.Substring(1).Replace('_', ' ');
        }
    }

    /// <summary>SSBU Arena, the competitive edition with the same account: its launcher writes the folder holding SSBU-Arena.exe
    /// to %LOCALAPPDATA%\SSBU Arena\install_path.txt (one line) at each start. Windows only (Arena has no Linux or macOS build):
    /// elsewhere only its site is linked.</summary>
    public static class ArenaEdition
    {
        public const string Site = "https://www.ssbuarena.com";
        public const string DownloadPage = "https://www.ssbuarena.com/download";
        static readonly string[] ExeNames = { "SSBU-Arena.exe", "SSBU Arena.exe" };   // (the release's; a dev build's)

        public static string PathFile => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SSBU Arena", "install_path.txt");

        /// <summary>Arena's launcher named by <paramref name="file"/> (default PathFile) when the exe is there; null otherwise
        /// (not Windows, no file, the folder moved). Never throws.</summary>
        public static string Find(string file = null, bool? windows = null)
        {
            if (!(windows ?? OperatingSystem.IsWindows())) return null;
            try
            {
                file ??= PathFile;
                if (!File.Exists(file)) return null;
                string folder = (File.ReadLines(file, Encoding.UTF8).FirstOrDefault() ?? "").Trim().Trim('"');
                if (folder == "" || folder.Length > 1024 || folder.IndexOfAny(Path.GetInvalidPathChars()) >= 0 || !Path.IsPathRooted(folder)) return null;
                foreach (string n in ExeNames)
                {
                    string exe = Path.Combine(folder, n);
                    if (File.Exists(exe)) return Path.GetFullPath(exe);
                }
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is ArgumentException || e is NotSupportedException || e is System.Security.SecurityException) { }
            return null;
        }

        /// <summary>the card's click: Arena's launcher started from its folder when installed, else the download page opened
        /// with <paramref name="openUrl"/>. What was done, for the log.</summary>
        public static string Open(Action<string> openUrl)
        {
            string exe = Find();
            if (exe != null)
            {
                try
                {
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(exe) { WorkingDirectory = Path.GetDirectoryName(exe), UseShellExecute = true });
                    return "SSBU Arena: started " + exe;
                }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception || e is InvalidOperationException || e is IOException) { openUrl?.Invoke(DownloadPage); return "SSBU Arena: " + exe + " did not start (" + e.Message + "): its download page"; }
            }
            openUrl?.Invoke(DownloadPage);
            return "SSBU Arena: not installed here: its download page";
        }
    }
}
