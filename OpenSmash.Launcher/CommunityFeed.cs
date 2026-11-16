using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Launcher
{
    // The lobby's public, read-only pages (tools/nex/board.py): /status.json (players online now, rooms, open rooms:
    // counts only) and /leaderboard.json (each ranked player's opaque id, position, best GSP and main fighter; a name
    // only for a player who chose to show one). The home page's right column shows them, asked again every minute.
    sealed class LobbyStatus
    {
        public int Online, Rooms, Arenas, OpenRooms, Playing, Ranked;
    }

    sealed class BoardRow
    {
        public int Rank, Fighter;
        public uint Gsp;
        public string Id = "", Name = "";
    }

    static class CommunityFeed
    {
        public const string Lobby = "opensmash-lobby.duckdns.org:39421";
        public const string SitePage = "https://denkishi.github.io/opensmash-site/leaderboard/";
        public static readonly TimeSpan Every = TimeSpan.FromSeconds(60);

        static int Int(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n) ? Math.Max(0, n) : 0;

        public static LobbyStatus? ParseStatus(string json)
        {
            try
            {
                using var j = JsonDocument.Parse(json);
                var r = j.RootElement;
                if (r.ValueKind != JsonValueKind.Object || !r.TryGetProperty("online", out _)) return null;
                return new LobbyStatus
                {
                    Online = Int(r, "online"), Rooms = Int(r, "rooms"), Arenas = Int(r, "arenas"),
                    OpenRooms = Int(r, "open_rooms"), Playing = Int(r, "playing"), Ranked = Int(r, "ranked"),
                };
            }
            catch (JsonException) { return null; }
        }

        // The first `take` players, best first, and how many are ranked; null when the text is not a leaderboard.
        public static (List<BoardRow> Top, int Ranked)? ParseBoard(string json, int take = 10)
        {
            try
            {
                using var j = JsonDocument.Parse(json);
                var r = j.RootElement;
                if (r.ValueKind != JsonValueKind.Object || !r.TryGetProperty("players", out var players) || players.ValueKind != JsonValueKind.Array) return null;
                var rows = new List<BoardRow>();
                foreach (var p in players.EnumerateArray())
                {
                    if (p.ValueKind != JsonValueKind.Object) continue;
                    var row = new BoardRow
                    {
                        Rank = Int(p, "rank"), Fighter = p.TryGetProperty("fighter", out var f) && f.TryGetInt32(out int k) ? k : -1,
                        Gsp = p.TryGetProperty("gsp", out var g) && g.TryGetUInt32(out uint gsp) ? gsp : 0,
                        Id = p.TryGetProperty("id", out var id) && id.ValueKind == JsonValueKind.String ? id.GetString() ?? "" : "",
                        Name = p.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String ? Clean(n.GetString() ?? "") : "",
                    };
                    if (row.Rank > 0) rows.Add(row);
                }
                rows.Sort((a, b) => a.Rank.CompareTo(b.Rank));
                int ranked = Int(r, "ranked");
                return (rows.GetRange(0, Math.Min(take, rows.Count)), Math.Max(ranked, rows.Count));
            }
            catch (JsonException) { return null; }
        }

        // The player's own row (by the opaque id the game's NSA id gives, DiscordAccount.PlayerId), or null: not ranked.
        public static BoardRow? Mine(string json, string playerId)
        {
            if (playerId == "") return null;
            var all = ParseBoard(json, int.MaxValue);
            return all?.Top.Find(r => r.Id == playerId);
        }

        // A shown name as one short line (the lobby already limits it to 32 characters).
        public static string Clean(string name)
        {
            var sb = new System.Text.StringBuilder();
            foreach (char c in name) if (!char.IsControl(c)) sb.Append(c);
            string s = sb.ToString().Trim();
            return s.Length > 32 ? s.Substring(0, 32) : s;
        }

        // What a row shows: the player's chosen name, else their main fighter's.
        public static string Who(BoardRow row) => row.Name != "" ? row.Name : DiscordAccount.FighterName(row.Fighter) + " player";

        public static async Task<(LobbyStatus? Status, string? Board)> Fetch(HttpClient http, CancellationToken cancel)
        {
            async Task<string?> Get(string path)
            {
                try
                {
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    timeout.CancelAfter(TimeSpan.FromSeconds(8));
                    using var r = await http.GetAsync($"http://{Lobby}{path}", timeout.Token);
                    return r.IsSuccessStatusCode ? await r.Content.ReadAsStringAsync(timeout.Token) : null;
                }
                catch (Exception e) when (e is HttpRequestException or TaskCanceledException or OperationCanceledException or InvalidOperationException) { return null; }
            }
            var status = Get("/status.json");
            var board = Get("/leaderboard.json");
            string? s = await status;
            return (s == null ? null : ParseStatus(s), await board);
        }
    }
}
