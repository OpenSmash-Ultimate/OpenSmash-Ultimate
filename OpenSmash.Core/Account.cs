// SPDX-License-Identifier: GPL-3.0-or-later
// OpenSmash accounts: the SSBU Arena accounts service (one Supabase project for both games), with Discord as the
// identity. Sign-in in the player's browser (RFC 8252, a loopback redirect to http://127.0.0.1:<random port>):
// - default: OpenSmash's accounts site (AccountService.DefaultSignInPage, https://account.opensmashultimate.com/launcher),
//   where the player signs in with Discord or with e-mail and password; the page sends the browser back with
//   ?state=..&refresh_token=.. ("sign_in_page" in account_service.json or OPENSMASH_SIGNIN_PAGE names another page);
// - without a sign-in page ("sign_in_page": "direct", or a service of one's own set by the install's config or
//   OPENSMASH_SUPABASE_URL): Supabase's own OAuth with PKCE (provider discord). The browser comes back with ?code=...,
//   which only this launcher can turn into a session (it alone has the code verifier). Supabase must allow the redirect
//   http://127.0.0.1:*/** (Auth > URL Configuration > Redirect URLs).
// The session (the refresh token) is kept for this user only: DPAPI on Windows, a file only the user can read elsewhere.
// The lobby ticket ("<user id>|<name>|<expiry>.<HMAC>", public.lobby_ticket) lets the game into the official lobby.
// The publishable key is meant for clients: what it can do is decided by Row Level Security and the database functions.
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class Profile
    {
        public string Id, Email, Username, DiscordId;
        public int Tag;
        public string Display => Username + "#" + Tag.ToString("0000");
    }

    public sealed class AccountException : Exception
    {
        public int Status; public string Body;
        public AccountException(string message, int status = 0, string body = null) : base(message) { Status = status; Body = body ?? ""; }
    }

    public sealed class AccountService
    {
        public string Url, Key, SignInPage, AccountPage;
        public bool Configured => !string.IsNullOrEmpty(Url) && !string.IsNullOrEmpty(Key);
        /// <summary>OpenSmash's accounts site: the sign-in page the launcher opens (Discord, or e-mail and password) and the
        /// player's account page. They belong to the accounts service built into the release.</summary>
        public const string DefaultSignInPage = "https://account.opensmashultimate.com/launcher";
        public const string DefaultAccountPage = "https://account.opensmashultimate.com/account";
        /// <summary>"sign_in_page": "direct" = no sign-in page: Discord straight from Supabase (PKCE)</summary>
        public const string Direct = "direct";
        bool ownService;          // the service comes from the install's config or the environment, not from the build

        /// <summary>OPENSMASH_SUPABASE_URL / _KEY, else config/account_service.json of the install, else the one built in</summary>
        public static AccountService Load(string root)
        {
            var s = new AccountService();
            try
            {
                using var st = Assembly.GetExecutingAssembly().GetManifestResourceStream("OpenSmash.Core.account_service.json");
                if (st != null) s.Read(new StreamReader(st).ReadToEnd());
            }
            catch { }
            try
            {
                string f = root == null ? null : Path.Combine(root, "config", "account_service.json");
                if (f != null && File.Exists(f)) s.Read(File.ReadAllText(f), true);
            }
            catch { }
            string envUrl = Environment.GetEnvironmentVariable("OPENSMASH_SUPABASE_URL");
            if (!string.IsNullOrEmpty(envUrl)) { s.Url = envUrl; s.ownService = true; s.SignInPage = s.AccountPage = null; }
            s.Key = Environment.GetEnvironmentVariable("OPENSMASH_SUPABASE_KEY") ?? s.Key;
            s.SignInPage = Environment.GetEnvironmentVariable("OPENSMASH_SIGNIN_PAGE") ?? s.SignInPage;
            s.Url = s.Url?.TrimEnd('/');
            // the accounts site goes with the built-in service; a service of one's own (a test server) has no page
            // unless it names one
            s.SignInPage = Page(s.SignInPage, s.ownService ? null : DefaultSignInPage);
            s.AccountPage = Page(s.AccountPage, s.ownService ? null : DefaultAccountPage);
            return s;
        }

        /// <summary>a page as configured: "direct" = none; a real one must be https (or this PC, for tests), because the
        /// sign-in page is trusted with the player's sign-in; anything else counts as not given</summary>
        internal static string Page(string value, string fallback)
        {
            if (string.IsNullOrWhiteSpace(value)) return fallback;
            value = value.Trim();
            if (string.Equals(value, Direct, StringComparison.OrdinalIgnoreCase)) return null;
            if (!Uri.TryCreate(value, UriKind.Absolute, out var u) || u.Fragment.Length > 0) return fallback;
            bool local = u.Scheme == Uri.UriSchemeHttp && (u.Host == "127.0.0.1" || u.Host == "localhost");
            return u.Scheme == Uri.UriSchemeHttps || local ? value : fallback;
        }

        void Read(string json, bool install = false)
        {
            using var d = JsonDocument.Parse(json);
            string Get(string n) => d.RootElement.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.String && v.GetString().Length > 0 ? v.GetString() : null;
            // (the install's file naming a service: its pages are the ones it names, not the built-in service's)
            if (install && Get("url") != null) { ownService = true; SignInPage = AccountPage = null; }
            Url = Get("url") ?? Url; Key = Get("publishable_key") ?? Key;
            SignInPage = Get("sign_in_page") ?? SignInPage; AccountPage = Get("account_page") ?? AccountPage;
        }
    }

    public sealed class Account
    {
        public readonly AccountService Service;
        readonly string sessionFile;
        readonly HttpClient http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        string access, refresh;
        DateTime accessUntil;
        public Profile Me { get; private set; }
        /// <summary>signed in: a session AND its profile read (between the two, Me has an id but no username yet)</summary>
        public bool SignedIn => Me != null && !string.IsNullOrEmpty(Me.Username);
        /// <summary>opens a URL in the player's browser (the launcher's own way; tests catch it)</summary>
        public Action<string> OpenBrowser = url => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });

        public Account(string root, AccountService service = null)
        {
            Service = service ?? AccountService.Load(root);
            string dir = Path.Combine(root ?? Path.GetDirectoryName(Updater.ExePath), "config");
            sessionFile = Path.Combine(dir, "session.dat");
            try { http.DefaultRequestHeaders.UserAgent.ParseAdd("OpenSmash-Launcher/1.0"); } catch { }
        }

        /// <summary>the last session of this PC: refreshed and its profile read. false (no exception) when there is none or
        /// it was revoked; error has the text when the server could not be reached.</summary>
        public async Task<bool> ResumeAsync(Action<string> error = null)
        {
            string rt = LoadRefresh();
            if (rt == null || !Service.Configured) return false;
            try { await TokenAsync("refresh_token", new Dictionary<string, string> { ["refresh_token"] = rt }); await LoadProfileAsync(); return true; }
            catch (AccountException e) when (e.Status == 400 || e.Status == 401) { SignOutLocal(); return false; }
            catch (Exception e) { error?.Invoke(e.Message); return false; }
        }

        /// <summary>sign-in in the browser (the accounts site: Discord or e-mail; without a sign-in page, Discord straight
        /// from Supabase); throws AccountException with a message for the player</summary>
        public async Task SignInWithBrowserAsync(CancellationToken cancel)
        {
            if (!Service.Configured) throw new AccountException("Online accounts are not set up in this build of OpenSmash.");
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            try
            {
                int port = ((IPEndPoint)listener.LocalEndpoint).Port;
                string state = RandomHex(24), verifier = RandomUrlSafe(48);
                string redirect = "http://127.0.0.1:" + port + "/callback/" + state;
                string url = Service.SignInPage != null
                    ? Service.SignInPage + (Service.SignInPage.Contains('?') ? "&" : "?") + "port=" + port + "&state=" + state + "&provider=discord"
                    : Service.Url + "/auth/v1/authorize?provider=discord&redirect_to=" + Uri.EscapeDataString(redirect) +
                      "&code_challenge=" + Challenge(verifier) + "&code_challenge_method=s256";
                OpenBrowser(url);
                var until = DateTime.UtcNow.AddMinutes(10);
                while (true)
                {
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    timeout.CancelAfter(until - DateTime.UtcNow);
                    TcpClient c;
                    try { c = await listener.AcceptTcpClientAsync(timeout.Token); }
                    catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { throw new AccountException("The sign-in in the browser took too long: try again."); }
                    using (c)
                    using (var stream = c.GetStream())
                    {
                        stream.ReadTimeout = 5000;
                        string line = new StreamReader(stream, Encoding.ASCII).ReadLine() ?? "";
                        var cb = Callback.Parse(line, state);
                        bool ours = cb.Code != null || cb.RefreshToken != null;
                        string text = ours ? "Signed in: go back to the OpenSmash launcher. You can close this tab."
                                    : cb.Error != null ? "The sign-in did not work: " + cb.Error : "This is the OpenSmash launcher's sign-in: nothing to do here.";
                        Reply(stream, text);
                        if (cb.Error != null) throw new AccountException("The sign-in did not work: " + cb.Error);
                        if (!ours) continue;          // (a stray request: favicon, another page)
                        if (cb.Code != null)
                            await TokenAsync("pkce", new Dictionary<string, string> { ["auth_code"] = cb.Code, ["code_verifier"] = verifier });
                        else
                            await TokenAsync("refresh_token", new Dictionary<string, string> { ["refresh_token"] = cb.RefreshToken });
                        await LoadProfileAsync();
                        return;
                    }
                }
            }
            finally { listener.Stop(); }
        }

        /// <summary>what came back to the loopback address: "GET /callback/&lt;state&gt;?code=.. HTTP/1.1" (Supabase's PKCE) or
        /// "GET /callback?state=..&amp;refresh_token=.." (a sign-in page); anything else is not ours</summary>
        public sealed class Callback
        {
            public string Code, RefreshToken, Error;
            public static Callback Parse(string requestLine, string state)
            {
                var r = new Callback();
                var parts = requestLine.Split(' ');
                if (parts.Length < 2 || parts[0] != "GET") return r;
                string target = parts[1], path = target, query = "";
                int q = target.IndexOf('?');
                if (q >= 0) { path = target.Substring(0, q); query = target.Substring(q + 1); }
                var args = new Dictionary<string, string>();
                foreach (var kv in query.Split('&', StringSplitOptions.RemoveEmptyEntries))
                {
                    int e = kv.IndexOf('=');
                    if (e > 0) args[Uri.UnescapeDataString(kv.Substring(0, e).Replace('+', ' '))] = Uri.UnescapeDataString(kv.Substring(e + 1).Replace('+', ' '));
                }
                bool pathState = path == "/callback/" + state, queryState = path == "/callback" && args.TryGetValue("state", out var s) && s == state;
                if (!pathState && !queryState) return r;
                if (args.TryGetValue("error_description", out var err) || args.TryGetValue("error", out err)) { r.Error = err.Length > 200 ? err.Substring(0, 200) : err; return r; }
                if (pathState && args.TryGetValue("code", out var code) && code.Length > 0) r.Code = code;
                if (queryState && args.TryGetValue("refresh_token", out var rt) && rt.Length > 0) r.RefreshToken = rt;
                return r;
            }
        }

        static void Reply(Stream s, string text)
        {
            string html = "<!doctype html><meta charset=utf-8><title>OpenSmash</title><body style=\"background:#10131a;color:#eef1f6;" +
                          "font:18px system-ui;display:grid;place-items:center;height:90vh\"><p>" + WebUtility.HtmlEncode(text) + "</p>";
            byte[] body = Encoding.UTF8.GetBytes(html);
            byte[] head = Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n" +
                                                  "Connection: close\r\nContent-Length: " + body.Length + "\r\n\r\n");
            s.Write(head, 0, head.Length);
            s.Write(body, 0, body.Length);
        }

        public async Task SignOutAsync()
        {
            if (access != null) try { await RequestAsync("POST", "/auth/v1/logout", "{}", access); } catch { }
            SignOutLocal();
        }

        void SignOutLocal()
        {
            access = refresh = null; Me = null;
            try { File.Delete(sessionFile); } catch { }
        }

        /// <summary>a fresh lobby ticket for the game (SSBU_ONLINE_TICKET_FILE)</summary>
        public async Task<string> LobbyTicketAsync()
        {
            await EnsureAccessAsync();
            return JsonSerializer.Deserialize<string>(await RequestAsync("POST", "/rest/v1/rpc/lobby_ticket", "{}", access));
        }

        /// <summary>a REST / RPC call with the player's own session (never a service key): the answer's text</summary>
        public async Task<string> RestAsync(string method, string path, string body) { await EnsureAccessAsync(); return await RequestAsync(method, path, body, access); }

        /// <summary>a public read with the publishable key only</summary>
        public Task<string> AnonAsync(string path) => RequestAsync("GET", path, null, Service.Key);

        public async Task<string> AccessTokenAsync() { await EnsureAccessAsync(); return access; }

        async Task EnsureAccessAsync()
        {
            if (access != null && DateTime.UtcNow < accessUntil) return;
            if (refresh == null) throw new AccountException("Not signed in.");
            await TokenAsync("refresh_token", new Dictionary<string, string> { ["refresh_token"] = refresh });
        }

        async Task TokenAsync(string grant, Dictionary<string, string> body)
        {
            string text = await RequestAsync("POST", "/auth/v1/token?grant_type=" + grant, JsonSerializer.Serialize(body), null);
            using var d = JsonDocument.Parse(text);
            var r = d.RootElement;
            access = r.GetProperty("access_token").GetString();
            refresh = r.GetProperty("refresh_token").GetString();
            accessUntil = DateTime.UtcNow.AddSeconds(r.GetProperty("expires_in").GetDouble() - 60);
            var user = r.GetProperty("user");
            string id = user.GetProperty("id").GetString();
            string email = user.TryGetProperty("email", out var e) && e.ValueKind == JsonValueKind.String ? e.GetString() : "";
            string discord = null;
            if (user.TryGetProperty("identities", out var ids) && ids.ValueKind == JsonValueKind.Array)
                foreach (var i in ids.EnumerateArray())
                    if (i.TryGetProperty("provider", out var p) && p.GetString() == "discord" && i.TryGetProperty("id", out var did))
                        discord = did.ValueKind == JsonValueKind.String ? did.GetString() : did.ToString();
            // (a refresh keeps the profile already read: SSBU Arena 0.2.29, an empty name went to the game)
            if (Me != null && Me.Id == id) { Me.Email = email; Me.DiscordId = discord ?? Me.DiscordId; }
            else Me = new Profile { Id = id, Email = email, DiscordId = discord };
            SaveRefresh(refresh);
        }

        /// <summary>signed in always means a username: a session without a readable profile is no session</summary>
        async Task LoadProfileAsync()
        {
            try
            {
                using var d = JsonDocument.Parse(await RequestAsync("GET", "/rest/v1/profiles?select=username,tag&id=eq." + Uri.EscapeDataString(Me.Id), null, access));
                if (d.RootElement.GetArrayLength() == 0) throw new AccountException("This account has no profile yet: open the account page once.");
                var row = d.RootElement[0];
                string name = row.TryGetProperty("username", out var u) && u.ValueKind == JsonValueKind.String ? u.GetString() : null;
                if (string.IsNullOrWhiteSpace(name)) throw new AccountException("This account has no username yet: choose one on the account page.");
                Me.Username = name;
                Me.Tag = row.TryGetProperty("tag", out var t) && t.ValueKind == JsonValueKind.Number ? t.GetInt32() : 0;
            }
            catch { Me = null; throw; }
        }

        async Task<string> RequestAsync(string method, string path, string body, string bearer)
        {
            using var rq = new HttpRequestMessage(new HttpMethod(method), Service.Url + path);
            rq.Headers.Add("apikey", Service.Key);
            if (bearer != null) rq.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", bearer);
            rq.Headers.Accept.ParseAdd("application/json");
            if (body != null) rq.Content = new StringContent(body, Encoding.UTF8, "application/json");
            HttpResponseMessage rs;
            try { rs = await http.SendAsync(rq); }
            catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException)
            {
                string detail = $"{e.GetType().Name}: {e.Message}" + (e.InnerException != null ? $" ({e.InnerException.Message})" : "");
                Console.WriteLine($"[Account.RequestAsync] {method} {Service.Url + path} failed: {detail}");
                try { File.AppendAllText("build/launcher.log", $"[Account.RequestAsync] {method} {Service.Url + path} failed: {detail}\n"); } catch { }
                throw new AccountException("Cannot reach the accounts server: " + detail);
            }
            using (rs)
            {
                string text = await rs.Content.ReadAsStringAsync();
                if (!rs.IsSuccessStatusCode)
                {
                    Console.WriteLine($"[Account.RequestAsync] Status {(int)rs.StatusCode} from {method} {Service.Url + path}: {text}");
                    try { File.AppendAllText("build/launcher.log", $"[Account.RequestAsync] Status {(int)rs.StatusCode} from {method} {Service.Url + path}: {text}\n"); } catch { }
                    throw new AccountException("The accounts server answered " + (int)rs.StatusCode, (int)rs.StatusCode, text);
                }
                return text;
            }
        }

        internal static string RandomHex(int bytes) => Convert.ToHexString(RandomNumberGenerator.GetBytes(bytes)).ToLowerInvariant();
        internal static string RandomUrlSafe(int bytes) => B64Url(RandomNumberGenerator.GetBytes(bytes));
        static string B64Url(byte[] b) => Convert.ToBase64String(b).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        /// <summary>PKCE S256 (RFC 7636)</summary>
        public static string Challenge(string verifier) => B64Url(SHA256.HashData(Encoding.ASCII.GetBytes(verifier)));

        // ---- the session, for this user only -------------------------------------------------------------------------
        static readonly byte[] Entropy = Encoding.ASCII.GetBytes("opensmash session");

        void SaveRefresh(string token)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(sessionFile));
                byte[] data = Encoding.UTF8.GetBytes(token);
                if (OperatingSystem.IsWindows())
                    File.WriteAllBytes(sessionFile, ProtectedData.Protect(data, Entropy, DataProtectionScope.CurrentUser));
                else
                {
                    using (File.Create(sessionFile)) { }
                    File.SetUnixFileMode(sessionFile, UnixFileMode.UserRead | UnixFileMode.UserWrite);
                    File.WriteAllBytes(sessionFile, data);
                }
            }
            catch { }
        }

        string LoadRefresh()
        {
            try
            {
                if (!File.Exists(sessionFile)) return null;
                byte[] b = File.ReadAllBytes(sessionFile);
                if (OperatingSystem.IsWindows()) b = ProtectedData.Unprotect(b, Entropy, DataProtectionScope.CurrentUser);
                string t = Encoding.UTF8.GetString(b).Trim();
                return t.Length > 0 ? t : null;
            }
            catch { return null; }
        }
    }
}
