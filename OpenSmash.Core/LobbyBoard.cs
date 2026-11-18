// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.0) A signed-in player's own GSP and place on the official lobbies' leaderboard (tools/nex/board.py): GET /gsp/me
// with the lobby ticket as the proof (header X-OpenSmash-Ticket), not an install's NSA signature. The lobby keeps the
// game's records under the account (one identity on every PC), lists it under the ticket's name, and merges it with the
// rows of the installs linked before to the same Discord account. The ticket is sent to the official lobbies only
// (OfficialLobby.All): it is a credential, never for a server of one's own.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class BoardFighter
    {
        public int Fighter;
        public uint Gsp;
        public int Rank;   // (0: not ranked)
    }

    public sealed class BoardMe
    {
        public string Id = "";     // the player's row id on /leaderboard.json ("" none)
        public int Rank;           // overall position (0: not ranked)
        public int Ranked;         // players ranked on that lobby
        public string Name = "";
        /// <summary>the name shown on the leaderboard (board.py me()'s "show"; a lobby before it says nothing: shown, the
        /// default for a signed-in account)</summary>
        public bool Shown = true;
        public List<BoardFighter> Fighters = new List<BoardFighter>();   // highest GSP first
    }

    public static class LobbyBoard
    {
        public const string TicketHeader = "X-OpenSmash-Ticket";

        /// <summary>a /gsp/me answer; null when it is not one</summary>
        public static BoardMe ParseMe(string json)
        {
            try
            {
                using var d = JsonDocument.Parse(json);
                var r = d.RootElement;
                if (r.ValueKind != JsonValueKind.Object) return null;
                var me = new BoardMe { Id = Str(r, "id"), Rank = Int(r, "rank"), Ranked = Int(r, "ranked"), Name = Str(r, "name"),
                                     Shown = !(r.TryGetProperty("show", out var sh) && sh.ValueKind == JsonValueKind.False) };
                if (r.TryGetProperty("fighters", out var fs) && fs.ValueKind == JsonValueKind.Array)
                    foreach (var f in fs.EnumerateArray())
                        if (f.ValueKind == JsonValueKind.Object && f.TryGetProperty("fighter", out var fk) && fk.ValueKind == JsonValueKind.Number && fk.TryGetInt32(out int kind)
                            && f.TryGetProperty("gsp", out var g) && g.ValueKind == JsonValueKind.Number && g.TryGetUInt32(out uint gsp))
                            me.Fighters.Add(new BoardFighter { Fighter = kind, Gsp = gsp, Rank = Int(f, "rank") });
                me.Fighters = me.Fighters.OrderByDescending(f => f.Gsp).ThenBy(f => f.Fighter).ToList();
                return me;
            }
            catch (JsonException) { return null; }
        }

        static int Int(JsonElement o, string name) =>
            o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n) ? n : 0;

        static string Str(JsonElement o, string name) =>
            o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

        /// <summary>several lobbies' answers as one: the first (the lobby played on) gives the row, the place and the name;
        /// each fighter's highest GSP of them all (with that lobby's place for it)</summary>
        public static BoardMe Merge(IEnumerable<BoardMe> answers)
        {
            BoardMe first = null;
            var best = new Dictionary<int, BoardFighter>();
            foreach (var me in answers)
            {
                if (me == null) continue;
                first ??= me;
                foreach (var f in me.Fighters)
                    if (!best.TryGetValue(f.Fighter, out var old) || f.Gsp > old.Gsp) best[f.Fighter] = f;
            }
            var all = first ?? new BoardMe();
            return new BoardMe
            {
                Id = all.Id, Rank = all.Rank, Ranked = all.Ranked, Name = all.Name, Shown = all.Shown,
                Fighters = best.Values.OrderByDescending(f => f.Gsp).ThenBy(f => f.Fighter).ToList(),
            };
        }

        /// <summary>GET /gsp/me with the ticket on each lobby (the one played on first), merged; null when none answered</summary>
        public static async Task<BoardMe> MeAsync(HttpClient http, IEnumerable<string> lobbies, string ticket, CancellationToken cancel)
        {
            if (string.IsNullOrEmpty(ticket)) return null;
            var asks = lobbies.Select(async lobby =>
            {
                try
                {
                    using var req = new HttpRequestMessage(HttpMethod.Get, $"http://{lobby}/gsp/me");
                    req.Headers.TryAddWithoutValidation(TicketHeader, ticket);
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    timeout.CancelAfter(TimeSpan.FromSeconds(8));
                    using var r = await http.SendAsync(req, timeout.Token);
                    return r.IsSuccessStatusCode ? ParseMe(await r.Content.ReadAsStringAsync(timeout.Token)) : null;
                }
                catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is InvalidOperationException) { return null; }
            }).ToList();
            var got = await Task.WhenAll(asks);
            return got.Any(m => m != null) ? Merge(got) : null;
        }

        /// <summary>The account's name shown on (or hidden from) the leaderboard: POST /gsp/profile with the ticket as the
        /// proof, on each official lobby (OfficialLobby.All: never another server, the ticket is a credential). A name
        /// shows, "" hides (board.py set_profile; the lobby takes the ticket's name, not this one). How many lobbies
        /// took it.</summary>
        public static async Task<int> SetShownAsync(HttpClient http, IEnumerable<string> lobbies, string ticket, bool shown, CancellationToken cancel)
        {
            if (string.IsNullOrEmpty(ticket)) return 0;
            string body = JsonSerializer.Serialize(new Dictionary<string, string> { ["name"] = shown ? "account" : "", ["discord"] = "" });
            var posts = lobbies.Select(async lobby =>
            {
                try
                {
                    using var req = new HttpRequestMessage(HttpMethod.Post, $"http://{lobby}/gsp/profile") { Content = new StringContent(body, System.Text.Encoding.UTF8, "application/json") };
                    req.Headers.TryAddWithoutValidation(TicketHeader, ticket);
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    timeout.CancelAfter(TimeSpan.FromSeconds(8));
                    using var r = await http.SendAsync(req, timeout.Token);
                    return r.IsSuccessStatusCode ? 1 : 0;
                }
                catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is InvalidOperationException) { return 0; }
            }).ToList();
            return (await Task.WhenAll(posts)).Sum();
        }

        /// <summary>the lobby ticket the launcher last wrote for the game (OnlinePass.File) while it has at least margin
        /// seconds left; null otherwise. For a launcher that cannot ask the accounts service itself.</summary>
        public static string StoredTicket(string root, long margin = 60, long now = 0)
        {
            try
            {
                string f = Path.Combine(root, "build", "cache", "online_pass.txt");
                if (!File.Exists(f)) return null;
                string t = File.ReadAllText(f).Trim();
                if (now == 0) now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
                return OnlinePass.Expiry(t) > now + margin ? t : null;
            }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }
    }
}
