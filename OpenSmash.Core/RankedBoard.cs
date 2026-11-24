// SPDX-License-Identifier: GPL-3.0-or-later
// SSBU Arena's ranked leaderboard (the launchers' COMPETITIVE page): ranked is SSBU Arena's alone (OpenSmash has mastery
// points, no ranking), read from the accounts service both games share with the publishable key (anyone may call it):
//   POST /rest/v1/rpc/ranked_board {p_region, p_search, p_fighter, p_limit, p_offset} ->
//        [{rank, username, tag, avatar, rating (numeric), games, wins, losses, placed, tier, tier_family, tier_division,
//          tier_color "#RRGGBB", region ("eu" | "na-east" | "sa"), region_rank, fighters [{fighter (a display name), games, wins}],
//          total}]
// as ssbuarena.com's leaderboard reads it (arena-website src/lib/ranked-account.ts). Aggregates only: no account ids.
// A failure is no answer (null): the page shows that quietly and asks again later; an empty board is an answer.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class RankedFighter
    {
        public string Fighter = "";   // the board's display name ("Joker"); Fighters.Key gives the game key
        public int Games, Wins;
    }

    public sealed class RankedRow
    {
        public long Rank, RegionRank, Total;
        public string Username = "", Region = "", Tier = "", TierFamily = "";
        public int Tag, Games, Wins, Losses;
        public int? TierDivision;
        public string TierColor;   // "#RRGGBB" or null
        public double Rating;
        public bool Placed;
        public List<RankedFighter> Fighters = new List<RankedFighter>();
        public string Shown => Username + "#" + Tag.ToString("0000", CultureInfo.InvariantCulture);
    }

    public static class RankedBoard
    {
        public const string Rpc = "ranked_board";
        public const string LeaderboardPage = "https://www.ssbuarena.com/leaderboard";
        /// <summary>the region filter's choices: null = every region, then the Arena servers' own names</summary>
        public static readonly string[] Regions = { null, "eu", "na-east", "sa" };
        public static readonly string[] RegionLabels = { "ALL", "EU", "NA EAST", "SA" };

        static string Str(JsonElement o, string n, int max = 40)
        {
            if (!o.TryGetProperty(n, out var v) || v.ValueKind != JsonValueKind.String) return "";
            string s = (v.GetString() ?? "").Trim();
            return s.Length > max ? s.Substring(0, max) : s;
        }

        static long Long(JsonElement o, string n)
        {
            if (!o.TryGetProperty(n, out var v)) return 0;
            if (v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out long l)) return l;
            if (v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out double d)) return (long)d;
            if (v.ValueKind == JsonValueKind.String && long.TryParse(v.GetString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out long s)) return s;
            return 0;
        }

        static double Num(JsonElement o, string n)
        {
            if (!o.TryGetProperty(n, out var v)) return 0;
            if (v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out double d)) return d;
            if (v.ValueKind == JsonValueKind.String && double.TryParse(v.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out double s)) return s;
            return 0;
        }

        static int Int(JsonElement o, string n) => (int)Math.Clamp(Long(o, n), int.MinValue, int.MaxValue);

        /// <summary>a "#RRGGBB" colour, else null</summary>
        public static string Color(string v) =>
            v != null && v.Length == 7 && v[0] == '#' && v.Skip(1).All(Uri.IsHexDigit) ? v.ToUpperInvariant() : null;

        /// <summary>ranked_board's rows (unknown fields dropped, at most 3 fighters a row); null when the answer is not a list</summary>
        public static List<RankedRow> Parse(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                if (d.RootElement.ValueKind != JsonValueKind.Array) return null;
                var list = new List<RankedRow>();
                foreach (var r in d.RootElement.EnumerateArray())
                {
                    if (r.ValueKind != JsonValueKind.Object) continue;
                    string name = Mastery.Clean(Str(r, "username", 64));
                    if (name == "") continue;
                    var row = new RankedRow
                    {
                        Rank = Math.Max(0, Long(r, "rank")), Username = name, Tag = Math.Clamp(Int(r, "tag"), 0, 9999),
                        Rating = Num(r, "rating"), Games = Math.Max(0, Int(r, "games")), Wins = Math.Max(0, Int(r, "wins")), Losses = Math.Max(0, Int(r, "losses")),
                        Placed = r.TryGetProperty("placed", out var p) && p.ValueKind == JsonValueKind.True,
                        Tier = Str(r, "tier", 24), TierFamily = Str(r, "tier_family", 24), TierColor = Color(Str(r, "tier_color", 7)),
                        Region = Str(r, "region") is "eu" or "na-east" or "sa" ? Str(r, "region") : "", RegionRank = Math.Max(0, Long(r, "region_rank")),
                        Total = Math.Max(0, Long(r, "total")),
                    };
                    long div = Long(r, "tier_division");
                    row.TierDivision = div >= 1 && div <= 3 ? (int)div : (int?)null;
                    if (row.TierFamily == "") row.TierFamily = row.Tier;
                    if (r.TryGetProperty("fighters", out var fs) && fs.ValueKind == JsonValueKind.Array)
                        foreach (var f in fs.EnumerateArray())
                        {
                            if (f.ValueKind != JsonValueKind.Object || row.Fighters.Count >= 3) continue;
                            string fighter = Str(f, "fighter");
                            if (fighter != "") row.Fighters.Add(new RankedFighter { Fighter = fighter, Games = Math.Max(0, Int(f, "games")), Wins = Math.Max(0, Int(f, "wins")) });
                        }
                    list.Add(row);
                }
                return list;
            }
            catch (JsonException) { return null; }
        }

        /// <summary>the board's first rows for a region (null: every region); null when the service did not answer with one</summary>
        public static async Task<List<RankedRow>> TopAsync(Account account, string region, int limit = 10)
        {
            if (account == null || !account.Service.Configured) return null;
            if (region != null && !Regions.Contains(region)) region = null;
            try
            {
                string body = JsonSerializer.Serialize(new Dictionary<string, object> { ["p_region"] = region, ["p_search"] = null, ["p_fighter"] = null, ["p_limit"] = Math.Clamp(limit, 1, 50), ["p_offset"] = 0 });
                return Parse(await account.AnonRpcAsync(Rpc, body))?.Take(Math.Max(0, limit)).ToList();
            }
            catch (Exception e) when (e is AccountException || e is HttpRequestException || e is TaskCanceledException || e is InvalidOperationException) { return null; }
        }

        /// <summary>the rating as shown ("1,523")</summary>
        public static string RatingShown(RankedRow r) => Math.Round(r.Rating).ToString("N0", CultureInfo.InvariantCulture);
    }
}
