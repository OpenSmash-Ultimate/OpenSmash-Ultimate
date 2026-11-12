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
    // config\discord_token.txt (key=value lines; delete it to sign out). OAuth2 with PKCE: the Discord app "OpenSmash"
    // is a Public Client, so no secret is involved; the browser comes back to http://127.0.0.1/callback, which this
    // listens on for the one request. The game signs in to EOS with the same token. Also what the account page shows:
    // the player's GSP per fighter (nex_powers.bin, the records the game posts, beside the save folder) and their
    // position in the leaderboard (the site's leaderboard.json, made from EOS's leaderboard by its workflow).
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

    public static class DiscordAccount
    {
        public const string AppId = "1069217215540371466";
        public const string Redirect = "http://127.0.0.1/callback";
        const string Api = "https://discord.com/api";
        public const string LeaderboardUrl = "https://denkishi.github.io/opensmash-site/leaderboard.json";
        static readonly HttpClient Http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };

        public static string TokenPath(string repo) => Path.Combine(repo, "config", "discord_token.txt");
        static long Now => DateTimeOffset.UtcNow.ToUnixTimeSeconds();

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
                string url = "https://discord.com/oauth2/authorize?response_type=code&client_id=" + AppId +
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
                        ["client_id"] = AppId, ["grant_type"] = "authorization_code", ["code"] = code,
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
            if (t.Refresh == "") return null;
            try
            {
                t = await Exchange(new Dictionary<string, string> {
                    ["client_id"] = AppId, ["grant_type"] = "refresh_token", ["refresh_token"] = t.Refresh }, t, cancel);
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

        // ---- the player's GSP per fighter: nex_powers.bin (runtime/hle_online.c), 33-byte entries: u64 pid, then the record
        // the game posts (u8 fighter, u32 GSP, u32, u32, u64 date, u32). Of several players' (tests), the newest's.
        public const int Entry = 33;
        public static List<(int Fighter, uint Gsp)> Records(string repo)
        {
            var list = new List<(int, uint)>();
            string path = Path.Combine(repo, "nex_powers.bin");
            byte[] d;
            try { if (!File.Exists(path)) return list; d = File.ReadAllBytes(path); } catch (IOException) { return list; }
            return RecordsOf(d);
        }

        public static List<(int Fighter, uint Gsp)> RecordsOf(byte[] d)
        {
            var list = new List<(int, uint)>();
            ulong best = 0, newest = 0;
            bool any = false;
            for (int at = 0; at + Entry <= d.Length; at += Entry)
            {
                ulong pid = BitConverter.ToUInt64(d, at), when = BitConverter.ToUInt64(d, at + 8 + 13);
                if (!any || when > newest) { any = true; newest = when; best = pid; }
            }
            for (int at = 0; at + Entry <= d.Length; at += Entry)
                if (BitConverter.ToUInt64(d, at) == best) list.Add((d[at + 8], BitConverter.ToUInt32(d, at + 9)));
            return list.OrderByDescending(r => r.Item2).ToList();
        }

        // ---- leaderboard.json: the player's overall position and each fighter's (0: not ranked)
        public static async Task<(int Rank, Dictionary<int, int> Fighters)> Ranks(string discordId, CancellationToken cancel)
        {
            var fighters = new Dictionary<int, int>();
            if (discordId == "") return (0, fighters);
            string text = await Http.GetStringAsync(LeaderboardUrl, cancel);
            return RanksIn(text, discordId);
        }

        public static (int Rank, Dictionary<int, int> Fighters) RanksIn(string json, string discordId)
        {
            var fighters = new Dictionary<int, int>();
            using var j = JsonDocument.Parse(json);
            if (!j.RootElement.TryGetProperty("players", out var players) || players.ValueKind != JsonValueKind.Array) return (0, fighters);
            foreach (var p in players.EnumerateArray())
            {
                if (!p.TryGetProperty("discord_id", out var id) || id.ValueKind != JsonValueKind.String || id.GetString() != discordId) continue;
                int rank = p.TryGetProperty("rank", out var r) && r.ValueKind == JsonValueKind.Number ? r.GetInt32() : 0;
                if (p.TryGetProperty("fighters", out var f) && f.ValueKind == JsonValueKind.Object)
                    foreach (var kv in f.EnumerateObject())
                        if (int.TryParse(kv.Name, out int k) && kv.Value.ValueKind == JsonValueKind.Object && kv.Value.TryGetProperty("rank", out var fr) && fr.ValueKind == JsonValueKind.Number)
                            fighters[k] = fr.GetInt32();
                return (rank, fighters);
            }
            return (0, fighters);
        }

        static readonly string?[] Names = {
            "Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
            "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Sheik", "Zelda", "Dr. Mario", "Pichu",
            "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom", "Mr. Game & Watch", "Meta Knight",
            "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Squirtle", "Ivysaur", "Charizard", "Diddy Kong",
            "Lucas", "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf", "Villager", "Mega Man",
            "Wii Fit Trainer", "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt",
            "Ryu", "Ken", "Cloud", "Corrin", "Bayonetta", "Inkling", "Ridley", "Simon", "Richter", "King K. Rool",
            "Isabelle", "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", "Ice Climbers", null, null, null, null,
            null, "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth", "Min Min", "Steve", "Sephiroth",
            "Pyra", "Mythra", "Kazuya", "Sora" };   // (by fighter kind, as runtime/net_discord_core.c)

        public static string FighterName(int kind) =>
            kind == 114 ? "Pokemon Trainer" : kind >= 0 && kind < Names.Length && Names[kind] != null ? Names[kind]! : $"Fighter {kind}";
    }
}
