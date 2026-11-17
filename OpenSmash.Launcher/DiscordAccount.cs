using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Launcher
{
    // Sign-in with Discord, shared with the game (runtime/net_discord.c) and the web launcher through
    // config\discord_token.txt (key=value lines; delete it to sign out). OAuth2 with PKCE: the Discord application is a
    // Public Client, so no secret is involved; its id is build\discord.txt (a release puts it there:
    // tools/discord_fetch.py), and without it there is no sign-in. The browser comes back to http://127.0.0.1/callback,
    // which this listens on for the one request.
    // Also what the account card shows: the player's GSP per fighter, kept by the lobbies' own game server
    // (GET /gsp/me, tools/nex/board.py, signed with this install's NSA id from netplay_id.txt), and their global position
    // (the one public lobby's leaderboard.json: every GSP result goes through its game server). The GSP belongs to the Discord account: this install
    // is linked to it with POST /gsp/link (the access token is sent once, the lobby verifies it with Discord and keeps
    // nothing of it; show on/off is the choice config\discord_leaderboard.txt remembers).
    public sealed class DiscordToken
    {
        public string Access = "", Refresh = "", Scope = "", UserId = "", Username = "", DisplayName = "", Avatar = "";
        public long ExpiresAt;

        public bool Fresh(long now, long margin) => Access != "" && ExpiresAt > 0 && ExpiresAt - margin > now;
        public string Shown => DisplayName != "" ? DisplayName : Username;

        public static DiscordToken? Parse(string text)
        {
            var t = new DiscordToken();
            foreach (var raw in text.Split('\n'))
            {
                string line = raw.Trim();
                int eq = line.IndexOf('=');
                if (line.StartsWith("#") || eq < 0) continue;
                string k = line.Substring(0, eq).Trim(), v = line.Substring(eq + 1).Trim();
                switch (k)
                {
                    case "access_token": t.Access = v; break;
                    case "refresh_token": t.Refresh = v; break;
                    case "scope": t.Scope = v; break;
                    case "expires_at": long.TryParse(v, out t.ExpiresAt); break;
                    case "user_id": t.UserId = v; break;
                    case "username": t.Username = v; break;
                    case "display_name": t.DisplayName = v; break;
                    case "avatar": t.Avatar = v; break;
                }
            }
            return t.Access != "" || t.Refresh != "" ? t : null;
        }

        public string Format()
        {
            foreach (var v in new[] { Access, Refresh, Scope, UserId, Username, DisplayName, Avatar })
                if (v.Contains('\n') || v.Contains('\r')) throw new ArgumentException("a value with a line break");
            return "# OpenSmash: the Discord sign-in the game and the launchers share. Delete this file to sign out.\n" +
                   $"access_token={Access}\nrefresh_token={Refresh}\nexpires_at={ExpiresAt}\nscope={Scope}\n" +
                   $"user_id={UserId}\nusername={Username}\ndisplay_name={DisplayName}\navatar={Avatar}\n";
        }
    }

    public sealed class FighterGsp
    {
        public int Fighter;
        public uint Gsp;
        public int Rank;   // the fighter's global position (0: not known)
    }

    public static class DiscordAccount
    {
        public const string Redirect = "http://127.0.0.1/callback";
        const string Api = "https://discord.com/api";
        public const string LeaderboardUrl = "http://opensmash-lobby.duckdns.org:39421/leaderboard.json";
        public static readonly string[] PublicLobbies = { "opensmash-lobby.duckdns.org:39421" };   // (the one public lobby; the North American one is gone)
        static readonly HttpClient Http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };

        public static string TokenPath(string repo) => Path.Combine(repo, "config", "discord_token.txt");
        static string ShownPath(string repo) => Path.Combine(repo, "config", "discord_leaderboard.txt");
        static long Now => DateTimeOffset.UtcNow.ToUnixTimeSeconds();

        // build\discord.txt: "ApplicationId=<digits>" (or the digits alone); "" when there is none.
        public static string AppId(string repo)
        {
            try
            {
                string path = Path.Combine(WindowSmash.BuildDir(repo), "discord.txt");
                return File.Exists(path) ? ParseAppId(File.ReadAllText(path)) : "";
            }
            catch (IOException) { return ""; }
        }

        public static string ParseAppId(string text)
        {
            foreach (var raw in text.Split('\n'))
            {
                string line = raw.Trim();
                if (line == "" || line.StartsWith("#")) continue;
                int eq = line.IndexOf('=');
                if (eq >= 0 && line.Substring(0, eq).Trim() != "ApplicationId") continue;
                string v = eq >= 0 ? line.Substring(eq + 1).Trim() : line;
                if (v.Length >= 15 && v.Length <= 20 && v.All(char.IsAsciiDigit)) return v;
            }
            return "";
        }

        public static DiscordToken? Load(string repo)
        {
            try { return File.Exists(TokenPath(repo)) ? DiscordToken.Parse(File.ReadAllText(TokenPath(repo))) : null; }
            catch (IOException) { return null; }
        }

        public static void Save(string repo, DiscordToken t)
        {
            string path = TokenPath(repo), tmp = path + ".tmp";
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(tmp, t.Format(), new UTF8Encoding(false));
            File.Move(tmp, path, true);
        }

        public static void SignOut(string repo)
        {
            try { File.Delete(TokenPath(repo)); } catch (IOException) { }
        }

        static string B64Url(byte[] b) => Convert.ToBase64String(b).TrimEnd('=').Replace('+', '-').Replace('/', '_');

        // The browser's sign-in, start to end: a token (saved) or an exception with a message for the player.
        public static async Task<DiscordToken> SignIn(string repo, CancellationToken cancel)
        {
            string appId = AppId(repo);
            if (appId == "") throw new InvalidOperationException("This build has no Discord application (build\\discord.txt): update OpenSmash.");
            string verifier = B64Url(RandomNumberGenerator.GetBytes(48));
            string challenge = B64Url(SHA256.HashData(Encoding.ASCII.GetBytes(verifier)));
            string state = B64Url(RandomNumberGenerator.GetBytes(16));
            TcpListener listener;
            try { listener = new TcpListener(IPAddress.Loopback, 80); listener.Start(); }
            catch (SocketException)
            {
                throw new InvalidOperationException("Port 80 on this PC is in use, and Discord sends the browser back there (http://127.0.0.1/callback). Close the program using it and try again.");
            }
            try
            {
                string url = "https://discord.com/oauth2/authorize?response_type=code&client_id=" + appId +
                             "&scope=" + Uri.EscapeDataString("identify openid sdk.social_layer_presence") +
                             "&redirect_uri=" + Uri.EscapeDataString(Redirect) +
                             "&code_challenge=" + challenge + "&code_challenge_method=S256&state=" + state + "&prompt=consent";
                Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                timeout.CancelAfter(TimeSpan.FromMinutes(5));
                for (;;)
                {
                    using var client = await listener.AcceptTcpClientAsync(timeout.Token);
                    var stream = client.GetStream();
                    var buf = new byte[8192];
                    int n = await stream.ReadAsync(buf, timeout.Token);
                    string req = Encoding.ASCII.GetString(buf, 0, n);
                    string first = req.Split('\n')[0];   // GET /callback?code=...&state=... HTTP/1.1
                    string[] parts = first.Split(' ');
                    if (parts.Length < 2 || !parts[1].StartsWith("/callback")) { await Reply(stream, 404, "Not here."); continue; }
                    var q = Query(parts[1]);
                    if (!q.TryGetValue("state", out var st) || st != state) { await Reply(stream, 400, "This sign-in was not started by OpenSmash. Close this tab."); continue; }
                    if (!q.TryGetValue("code", out var code))
                    {
                        await Reply(stream, 200, "Sign-in cancelled. You can close this tab.");
                        throw new OperationCanceledException("Sign-in cancelled in Discord.");
                    }
                    await Reply(stream, 200, "Signed in to OpenSmash with Discord. You can close this tab.");
                    var t = await Exchange(new Dictionary<string, string> {
                        ["client_id"] = appId, ["grant_type"] = "authorization_code", ["code"] = code,
                        ["redirect_uri"] = Redirect, ["code_verifier"] = verifier }, null, cancel);
                    await FillUser(t, cancel);
                    Save(repo, t);
                    return t;
                }
            }
            finally { listener.Stop(); }
        }

        static Dictionary<string, string> Query(string target)
        {
            var d = new Dictionary<string, string>();
            int qm = target.IndexOf('?');
            if (qm < 0) return d;
            foreach (var kv in target.Substring(qm + 1).Split('&'))
            {
                int eq = kv.IndexOf('=');
                if (eq > 0) d[Uri.UnescapeDataString(kv.Substring(0, eq))] = Uri.UnescapeDataString(kv.Substring(eq + 1).Replace('+', ' '));
            }
            return d;
        }

        static async Task Reply(NetworkStream s, int code, string text)
        {
            string html = "<!doctype html><meta charset=utf-8><title>OpenSmash</title><body style=\"font:16px sans-serif;background:#111;color:#eee;padding:40px\">" +
                          WebUtility.HtmlEncode(text) + "</body>";
            byte[] body = Encoding.UTF8.GetBytes(html);
            string head = $"HTTP/1.1 {code} {(code == 200 ? "OK" : "Error")}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n";
            await s.WriteAsync(Encoding.ASCII.GetBytes(head));
            await s.WriteAsync(body);
        }

        static async Task<DiscordToken> Exchange(Dictionary<string, string> form, DiscordToken? old, CancellationToken cancel)
        {
            using var r = await Http.PostAsync(Api + "/oauth2/token", new FormUrlEncodedContent(form), cancel);
            string text = await r.Content.ReadAsStringAsync(cancel);
            if (!r.IsSuccessStatusCode) throw new InvalidOperationException($"Discord refused the sign-in ({(int)r.StatusCode}).");
            using var j = JsonDocument.Parse(text);
            var t = old ?? new DiscordToken();
            t.Access = j.RootElement.GetProperty("access_token").GetString() ?? "";
            if (j.RootElement.TryGetProperty("refresh_token", out var rt)) t.Refresh = rt.GetString() ?? t.Refresh;
            if (j.RootElement.TryGetProperty("scope", out var sc)) t.Scope = sc.GetString() ?? "";
            t.ExpiresAt = Now + (j.RootElement.TryGetProperty("expires_in", out var ex) ? ex.GetInt64() : 3600);
            return t;
        }

        static async Task FillUser(DiscordToken t, CancellationToken cancel)
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, Api + "/users/@me");
            req.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", t.Access);
            using var r = await Http.SendAsync(req, cancel);
            if (!r.IsSuccessStatusCode) return;
            using var j = JsonDocument.Parse(await r.Content.ReadAsStringAsync(cancel));
            string S(string k) => j.RootElement.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
            t.UserId = S("id"); t.Username = S("username"); t.DisplayName = S("global_name"); t.Avatar = S("avatar");
        }

        // A token good for a while: as it is, refreshed (and saved), or null (signed out, or refused: sign in again).
        public static async Task<DiscordToken?> Current(string repo, CancellationToken cancel)
        {
            var t = Load(repo);
            if (t == null) return null;
            if (t.Fresh(Now, 300)) return t;
            string appId = AppId(repo);
            if (t.Refresh == "" || appId == "") return null;
            try
            {
                t = await Exchange(new Dictionary<string, string> {
                    ["client_id"] = appId, ["grant_type"] = "refresh_token", ["refresh_token"] = t.Refresh }, t, cancel);
                await FillUser(t, cancel);
                Save(repo, t);
                return t;
            }
            catch (Exception e) when (e is HttpRequestException or InvalidOperationException or TaskCanceledException or IOException)
            {
                var again = Load(repo);   // (the game may have refreshed it meanwhile: each refresh token works once)
                return again != null && again.Fresh(Now, 60) ? again : null;
            }
        }

        public static string AvatarUrl(DiscordToken t)
        {
            if (t.UserId == "") return "";
            if (t.Avatar != "") return $"https://cdn.discordapp.com/avatars/{t.UserId}/{t.Avatar}.png?size=64";
            ulong.TryParse(t.UserId, out var id);
            return $"https://cdn.discordapp.com/embed/avatars/{(id >> 22) % 6}.png";
        }

        // ---- the player on the lobbies' game server (tools/nex/board.py) -------------------------------------------
        static string Hex(byte[] b) => Convert.ToHexString(b).ToLowerInvariant();
        static string Hash16(string text) => Hex(SHA256.HashData(Encoding.UTF8.GetBytes(text))).Substring(0, 16);

        // The install's NSA id as the game's token writes it (runtime/hle_env.c): netplay_id.txt's two numbers XORed,
        // 16 hex digits; "" before the game has played online.
        public static string NsaId(string repo)
        {
            try
            {
                string path = Path.Combine(repo, "netplay_id.txt");
                if (!File.Exists(path)) return "";
                return NsaOf(File.ReadAllText(path));
            }
            catch (IOException) { return ""; }
        }

        public static string NsaOf(string text)
        {
            var parts = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2 || !ulong.TryParse(parts[0], System.Globalization.NumberStyles.HexNumber, null, out ulong a) ||
                !ulong.TryParse(parts[1], System.Globalization.NumberStyles.HexNumber, null, out ulong b) || (a == 0 && b == 0)) return "";
            ulong id = a ^ b;
            return (id == 0 ? 1UL : id).ToString("x16");
        }

        public static string PlayerId(string nsa) => Hash16("opensmash-player\n" + nsa);
        public static string AccountHash(string discordUserId) => Hash16("opensmash-discord\n" + discordUserId);

        public static string Sign(string nsa, string what, string player, long t, string payload) =>
            Hex(HMACSHA256.HashData(Encoding.UTF8.GetBytes(nsa), Encoding.UTF8.GetBytes($"opensmash1\n{what}\n{player}\n{t}\n{payload}")));

        // The lobbies to ask: the public ones, and online_server.txt's own when it names another.
        public static List<string> Lobbies(string repo)
        {
            var list = new List<string>(PublicLobbies);
            string server = "";
            try
            {
                string path = WindowSmash.ConfigFile(repo, "online_server.txt");
                if (File.Exists(path)) server = File.ReadAllLines(path).Select(l => l.Trim()).FirstOrDefault(l => l != "" && !l.StartsWith("#")) ?? "";
            }
            catch (IOException) { }
            string host = server.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
            if (host != "" && !new[] { "auto", "na", "eu", "off", "lan" }.Contains(host.ToLowerInvariant()) && !host.StartsWith("smash.recomps.wtf", StringComparison.OrdinalIgnoreCase))
            {
                if (!host.Contains(':')) host += ":39420";
                if (!list.Contains(host, StringComparer.OrdinalIgnoreCase)) list.Add(host);
            }
            return list;
        }

        // GET /gsp/me on every lobby, merged: the highest GSP of each fighter, highest first. Empty when the game has not
        // played online yet or no lobby knows the player.
        public static async Task<List<FighterGsp>> Records(string repo, CancellationToken cancel)
        {
            string nsa = NsaId(repo);
            var best = new Dictionary<int, uint>();
            if (nsa == "") return new List<FighterGsp>();
            string player = PlayerId(nsa);
            var asks = Lobbies(repo).Select(async lobby =>
            {
                try
                {
                    long t = Now;
                    using var req = new HttpRequestMessage(HttpMethod.Get, $"http://{lobby}/gsp/me");
                    req.Headers.Add("X-OpenSmash-Player", player);
                    req.Headers.Add("X-OpenSmash-Time", t.ToString());
                    req.Headers.Add("X-OpenSmash-Sig", Sign(nsa, "me", player, t, ""));
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    timeout.CancelAfter(TimeSpan.FromSeconds(8));
                    using var r = await Http.SendAsync(req, timeout.Token);
                    return r.IsSuccessStatusCode ? await r.Content.ReadAsStringAsync(timeout.Token) : null;
                }
                catch (Exception e) when (e is HttpRequestException or TaskCanceledException or InvalidOperationException) { return null; }
            }).ToList();
            foreach (var text in await Task.WhenAll(asks))
                if (text != null) MergeMe(text, best);
            return best.Select(kv => new FighterGsp { Fighter = kv.Key, Gsp = kv.Value }).OrderByDescending(f => f.Gsp).ThenBy(f => f.Fighter).ToList();
        }

        public static void MergeMe(string json, Dictionary<int, uint> best)
        {
            try
            {
                using var j = JsonDocument.Parse(json);
                if (!j.RootElement.TryGetProperty("fighters", out var fs) || fs.ValueKind != JsonValueKind.Array) return;
                foreach (var f in fs.EnumerateArray())
                    if (f.TryGetProperty("fighter", out var k) && k.TryGetInt32(out int kind) && f.TryGetProperty("gsp", out var g) && g.TryGetUInt32(out uint gsp))
                        if (!best.TryGetValue(kind, out uint old) || gsp > old) best[kind] = gsp;
            }
            catch (JsonException) { }
        }

        // leaderboard.json: the player's global position and each fighter's (0: not ranked)
        public static async Task<(int Rank, Dictionary<int, int> Fighters)> Ranks(string repo, DiscordToken? t, CancellationToken cancel)
        {
            // (a row's id is the Discord account's hash: one row per account, however many installs are linked to it)
            if (t == null || t.UserId == "") return (0, new Dictionary<int, int>());
            string text = await Http.GetStringAsync(LeaderboardUrl, cancel);
            return RanksIn(text, AccountHash(t.UserId));
        }

        public static (int Rank, Dictionary<int, int> Fighters) RanksIn(string json, string playerId)
        {
            var fighters = new Dictionary<int, int>();
            using var j = JsonDocument.Parse(json);
            if (!j.RootElement.TryGetProperty("players", out var players) || players.ValueKind != JsonValueKind.Array) return (0, fighters);
            foreach (var p in players.EnumerateArray())
            {
                if (!p.TryGetProperty("id", out var id) || id.ValueKind != JsonValueKind.String || id.GetString() != playerId) continue;
                int rank = p.TryGetProperty("rank", out var r) && r.ValueKind == JsonValueKind.Number ? r.GetInt32() : 0;
                if (p.TryGetProperty("fighters", out var f) && f.ValueKind == JsonValueKind.Object)
                    foreach (var kv in f.EnumerateObject())
                        if (int.TryParse(kv.Name, out int k) && kv.Value.ValueKind == JsonValueKind.Object && kv.Value.TryGetProperty("rank", out var fr) && fr.ValueKind == JsonValueKind.Number)
                            fighters[k] = fr.GetInt32();
                return (rank, fighters);
            }
            return (0, fighters);
        }

        // ---- the player's Discord name on the leaderboard: their choice, sent to every lobby --------------------------
        public static bool ShowsName(string repo)
        {
            // (on unless the file says "off": a missing file is on)
            try { return !File.Exists(ShownPath(repo)) || !File.ReadAllText(ShownPath(repo)).Trim().Equals("off", StringComparison.OrdinalIgnoreCase); }
            catch (IOException) { return true; }
        }

        // Remembers the choice and tells the lobbies: POST /gsp/link with the token's access token (the lobby verifies it with
        // Discord), retried once with the NSA id as "key" when the lobby does not know this install yet. Without a token (or when
        // the lobby refuses it) a hide goes as the old profile call. Returns how many lobbies took it.
        public static async Task<int> SetShown(string repo, DiscordToken? t, bool shown, CancellationToken cancel)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ShownPath(repo))!);
            File.WriteAllText(ShownPath(repo), shown ? "on\n" : "off\n");
            string nsa = NsaId(repo);
            if (nsa == "") return 0;
            string token = t?.Access ?? "";
            var posts = Lobbies(repo).Select(async lobby =>
            {
                bool ok = false;
                if (token != "")
                {
                    var (code, error) = await PostJson($"http://{lobby}/gsp/link", LinkBody(nsa, token, shown, false), cancel);
                    if (code == 403 && error == "unknown player")
                        (code, error) = await PostJson($"http://{lobby}/gsp/link", LinkBody(nsa, token, shown, true), cancel);
                    ok = code == 200;
                }
                if (!ok && !shown) ok = (await PostJson($"http://{lobby}/gsp/profile", HideBody(nsa), cancel)).Code == 200;
                return ok ? 1 : 0;
            }).ToList();
            return (await Task.WhenAll(posts)).Sum();
        }

        static string LinkBody(string nsa, string token, bool shown, bool withKey)
        {
            string player = PlayerId(nsa);
            long now = Now;
            var d = new Dictionary<string, object> {
                ["player"] = player, ["t"] = now, ["token"] = token, ["show"] = shown,
                ["sig"] = Sign(nsa, "link", player, now, token + "\n" + (shown ? "1" : "0")) };
            if (withKey) d["key"] = nsa;
            return JsonSerializer.Serialize(d);
        }

        // (the old /gsp/profile with no name: hides an already linked player, no Discord token needed)
        static string HideBody(string nsa)
        {
            string player = PlayerId(nsa);
            long now = Now;
            return JsonSerializer.Serialize(new Dictionary<string, object> {
                ["player"] = player, ["t"] = now, ["name"] = "", ["discord"] = "", ["sig"] = Sign(nsa, "profile", player, now, "\n") });
        }

        // POST JSON: (HTTP status, the answer's "error" text or ""); (0, "") when the lobby can not be reached.
        static async Task<(int Code, string Error)> PostJson(string url, string body, CancellationToken cancel)
        {
            try
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                timeout.CancelAfter(TimeSpan.FromSeconds(8));
                using var r = await Http.PostAsync(url, new StringContent(body, Encoding.UTF8, "application/json"), timeout.Token);
                string error = "";
                try
                {
                    using var j = JsonDocument.Parse(await r.Content.ReadAsStringAsync(timeout.Token));
                    if (j.RootElement.ValueKind == JsonValueKind.Object && j.RootElement.TryGetProperty("error", out var e) && e.ValueKind == JsonValueKind.String)
                        error = e.GetString() ?? "";
                }
                catch (JsonException) { }
                return ((int)r.StatusCode, error);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or InvalidOperationException) { return (0, ""); }
        }

        static readonly string?[] Names = {
            "Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
            "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Ice Climbers", "Sheik", "Zelda", "Dr. Mario",
            "Pichu", "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom", "Mr. Game & Watch",
            "Meta Knight", "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Pokemon Trainer", "Diddy Kong", "Lucas",
            "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf", "Villager", "Mega Man", "Wii Fit Trainer",
            "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt", "Ryu",
            "Ken", "Cloud", "Corrin", "Bayonetta", "Richter", "Inkling", "Ridley", "King K. Rool", "Simon", "Isabelle",
            "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth",
            "Min Min", "Steve", "Sephiroth", "Pyra & Mythra", "Kazuya", "Sora" };
        // (by the GSP records' fighter number - DataStore 82's u8, the game's save_no in ui_chara_db, not the fighter
        // kind: 29 is Mr. Game & Watch, issue #389; the same list as runtime/net_discord_core.c)

        public static string FighterName(int kind) =>
            kind >= 0 && kind < Names.Length && Names[kind] != null ? Names[kind]! : $"Fighter {kind}";
    }
}
