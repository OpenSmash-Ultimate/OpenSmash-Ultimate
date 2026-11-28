// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.5) The setup error reports (OpenSmash.Core SetupReport.cs) and the stale game check (StaleGame.cs), without a network:
// the switches, the kinds, the error code, the cleaning, the size caps, the limits (3 a run, a code once in 24 h), the request
// against logs-send's schema (its schema.ts: product, kinds, sizes), what the server's answers become, and the test mode.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using OpenSmash.Core;

static class SetupReportChecks
{
    /// <summary>logs-send's checks (supabase/functions/logs-send/schema.ts of the accounts service), as C#</summary>
    public static string Schema(string json)
    {
        if (Encoding.UTF8.GetByteCount(json) > 1_300_000) return "too big (body)";
        using var doc = JsonDocument.Parse(json);
        var b = doc.RootElement;
        if (b.ValueKind != JsonValueKind.Object) return "bad request";
        string S(string n) => b.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        string product = S("product") ?? "arena";
        if (product != "arena" && product != "opensmash") return "unknown product";
        var kinds = new List<string> { "manual", "launcher_crash", "install_error" };
        if (product == "opensmash") kinds.AddRange(new[] { "update_error", "build_error", "stale_game", "game_start_error" });
        if (!kinds.Contains(S("kind") ?? "manual")) return "unknown kind";
        string text = S("text") ?? "";
        if (text.Trim().Length == 0) return "no logs";
        if (Encoding.UTF8.GetByteCount(text) > 1_200_000) return "too big";
        foreach (var (name, max) in new[] { ("exception", 400), ("version", 40), ("commit", 64), ("windows", 100), ("gpu", 200) })
            if (S(name) is string v && v.Length > max) return name + " longer than " + max + " (the server cuts it)";
        foreach (var p in b.EnumerateObject())
            if (!new[] { "product", "kind", "text", "note", "exception", "version", "commit", "windows", "gpu" }.Contains(p.Name)) return "unknown field " + p.Name;
        return "";
    }

    sealed class FakeServer : HttpMessageHandler
    {
        public readonly List<(HttpRequestMessage Request, string Body)> Calls = new();
        public HttpStatusCode Status = HttpStatusCode.OK;
        public string Answer = "{\"id\":\"0f1e2d3c-0000-4000-8000-000000000000\",\"code\":\"0F1E2D3C\"}";
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Calls.Add((request, await request.Content.ReadAsStringAsync(cancellationToken)));
            return new HttpResponseMessage(Status) { Content = new StringContent(Answer, Encoding.UTF8, "application/json") };
        }
    }

    public static async Task Run(Action<bool, string> check)
    {
        string was = Environment.GetEnvironmentVariable(SetupReport.Variable), wasTest = Environment.GetEnvironmentVariable(SetupReport.TestVariable),
               wasStale = Environment.GetEnvironmentVariable(StaleGame.Variable);
        string root = Path.Combine(Path.GetTempPath(), "os-core-setup-" + Guid.NewGuid().ToString("N"));
        try
        {
            Environment.SetEnvironmentVariable(SetupReport.Variable, null);
            Environment.SetEnvironmentVariable(SetupReport.TestVariable, null);
            Environment.SetEnvironmentVariable(StaleGame.Variable, null);
            Switches(check, root);
            Kinds(check);
            Codes(check);
            Cleaning(check, root);
            Payload(check, root);
            Limits(check, root);
            await Sending(check, root);
            Stale(check, root);
            Start(check, root);
        }
        finally
        {
            Environment.SetEnvironmentVariable(SetupReport.Variable, was);
            Environment.SetEnvironmentVariable(SetupReport.TestVariable, wasTest);
            Environment.SetEnvironmentVariable(StaleGame.Variable, wasStale);
            try { Directory.Delete(root, true); } catch (IOException) { }
        }
    }

    static void Switches(Action<bool, string> check, string root)
    {
        check(SetupReport.SwitchOn(null) && SetupReport.SwitchOn("") && SetupReport.SwitchOn("1") && !SetupReport.SwitchOn("0") && !SetupReport.SwitchOn(" Off ")
              && !SetupReport.SwitchOn("false"), "setup report: OPENSMASH_SETUP_REPORTS on unless 0/off/false");
        Directory.CreateDirectory(Path.Combine(root, "config"));
        check(SetupReport.Enabled(root) && SetupReport.Enabled(null), "setup report: on by default (an install, and before there is one)");
        SetupReport.SetSetting(root, false);
        check(!SetupReport.SettingOn(root) && !SetupReport.Enabled(root), "setup report: the setting off stops them");
        SetupReport.SetSetting(root, true);
        check(SetupReport.SettingOn(root) && SetupReport.Enabled(root), "setup report: the setting back on");
        File.WriteAllText(Path.Combine(root, "config", "crash_reports.txt"), "off");
        check(!SetupReport.Enabled(root), "setup report: the crash reports' setting off stops them too");
        File.Delete(Path.Combine(root, "config", "crash_reports.txt"));
        Environment.SetEnvironmentVariable(SetupReport.Variable, "0");
        check(!SetupReport.Enabled(root) && !SetupReport.Enabled(null), "setup report: OPENSMASH_SETUP_REPORTS=0 stops them everywhere");
        Environment.SetEnvironmentVariable(SetupReport.Variable, null);
    }

    static void Kinds(Action<bool, string> check)
    {
        check(SetupReport.KindFor("install", "game Your game files", "There is no prod.keys here.") == "install_error", "kind: a missing prod.keys is an install_error");
        check(SetupReport.KindFor("install", "s4 Build the game", "exit 1") == "build_error", "kind: the install's build step is a build_error");
        check(SetupReport.KindFor("install", "build", "") == "build_error", "kind: the step id build");
        check(SetupReport.KindFor("install", "", "The game rebuild stopped (exit 2).") == "build_error", "kind: an error that says the build stopped");
        check(SetupReport.KindFor("install", "", "error: arcropolis.c:12: undeclared identifier") == "build_error", "kind: ARCropolis' compile error");
        check(SetupReport.KindFor("update", null, "IOException 0x80070020: The process cannot access the file") == "update_error", "kind: a file in use during an update");
        check(SetupReport.KindFor("update", null, "LNK2019 unresolved external symbol") == "build_error", "kind: an update whose build failed to link");
        check(SetupReport.KindFor("bootstrap") == "install_error" && SetupReport.KindFor("first_run") == "install_error", "kind: Python's set-up and the first download");
        check(SetupReport.KindFor("rebuild") == "build_error" && SetupReport.KindFor("build") == "build_error", "kind: rebuilds");
        check(SetupReport.KindFor("launcher") == "launcher_crash" && SetupReport.KindFor("stale") == "stale_game" && SetupReport.KindFor("game_start") == "game_start_error",
              "kind: crash, stale, start");
        check(SetupReport.Kinds.All(k => SetupReport.KindFor(k switch { "install_error" => "install", "update_error" => "update", "build_error" => "build",
                                                                        "launcher_crash" => "launcher", "stale_game" => "stale", _ => "game_start" }) == k),
              "kind: every kind reachable");
    }

    static void Codes(Action<bool, string> check)
    {
        string a = SetupReport.Code("install_error", @"Copy failed: C:\Users\" + Environment.UserName + @"\Games\OpenSmash\dump at 10:22:33 (12 MB)");
        string b = SetupReport.Code("install_error", @"Copy failed: C:\Users\SomeoneElse\Games\OpenSmash\dump at 23:01:02 (450 MB)");
        check(a.Length == 8 && a.All(c => char.IsDigit(c) || (c >= 'A' && c <= 'F')), "code: 8 hex digits, upper case: " + a);
        check(a == b, "code: the same error on another PC, another time and size, has the same code: " + a + " " + b);
        check(SetupReport.Code("update_error", "Copy failed") != SetupReport.Code("install_error", "Copy failed"), "code: the kind is part of it");
        check(SetupReport.Code("install_error", "There is no prod.keys here.") != SetupReport.Code("install_error", "There is no title.keys here."), "code: another error, another code");
        check(SetupReport.Code("install_error", "first line\nsecond") == SetupReport.Code("install_error", "  first line  \n other"), "code: from the first line only");
        check(new SetupReportResult { Code = "ABCD1234", Sent = true }.Message.StartsWith("Error code ABCD1234 \u2014 this was sent to the developers"), "message: the player's line");
        check(new SetupReportResult { Code = "ABCD1234", Off = true }.Message.Contains("off") && new SetupReportResult { Code = "ABCD1234", Duplicate = true }.Message.Contains("already"),
              "message: off and duplicate say so");
        check(SetupReport.Describe(new IOException("The process cannot access the file", unchecked((int)0x80070020))) == "IOException 0x80070020: The process cannot access the file",
              "describe: a file in use with its HRESULT");
        check(SetupReport.Describe(new UnauthorizedAccessException("Access to the path is denied.")).StartsWith("UnauthorizedAccessException 0x80070005"), "describe: access denied");
        check(SetupReport.Describe(new InvalidOperationException("x\ny")) == "InvalidOperationException: x", "describe: one line, no HRESULT for other errors");
    }

    static void Cleaning(Action<bool, string> check, string root)
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string raw = "user " + Environment.UserName + " on " + Environment.MachineName + "\n" + Path.Combine(home, "Desktop", "x.log") + "\n" + Path.Combine(root, "build", "ssbu.exe") +
                     "\nmail someone.real@example.org from 93.184.216.34, room 10.13.0.2\n" +
                     "Authorization: Bearer abcdefghijklmnop.qrstuvwxyz\ntoken=eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJlLXZhbHVl\n" +
                     "apikey: sb_publishable_ABCDEFGHIJKLMNOP\npassword = hunter2hunter2\nhttps://x.test/cb?state=s3cr3t&code=abc&refresh_token=zzzzzz\n" +
                     @"D:\Users\Another Person\AppData\Local\Temp\a.txt" + "\n/home/someone/opensmash/tools\n";
        string t = SetupReport.Scrub(raw, root);
        check(!t.Contains(Environment.UserName + " on") && !t.Contains(Environment.MachineName) && !t.Contains(home) && !t.Contains(root), "scrub: user, PC, home and install folders: " + t);
        check(t.Contains("<install>") && t.Contains("<home>") && !t.Contains("someone.real@example.org") && !t.Contains("93.184.216.34") && t.Contains("10.13.0.2"),
              "scrub: e-mail and internet addresses, not the rooms' 10.x");
        check(!t.Contains("abcdefghijklmnop") && !t.Contains("eyJhbGci") && !t.Contains("sb_publishable_ABC") && !t.Contains("hunter2") && !t.Contains("s3cr3t") && !t.Contains("zzzzzz"),
              "scrub: tokens, keys, passwords, secrets in addresses: " + t);
        check(!t.Contains("Another Person") && !t.Contains("/home/someone/"), "scrub: other users' folders");
        string big = new string('x', 3 * 1024 * 1024);
        string bound = SetupReport.Bound(big, SetupReport.MaxText);
        check(Encoding.UTF8.GetByteCount(bound) <= SetupReport.MaxText && bound.Contains("[... cut ...]"), "cap: a 3 MB text within 512 KB");
        string wide = string.Concat(Enumerable.Repeat("\u00e8\u6f22", 400 * 1024));
        check(Encoding.UTF8.GetByteCount(SetupReport.Bound(wide, SetupReport.MaxText)) <= SetupReport.MaxText, "cap: counted in UTF-8 bytes");
    }

    static string Install(string root)
    {
        Directory.CreateDirectory(Path.Combine(root, "tools"));
        Directory.CreateDirectory(Path.Combine(root, "build", "logs"));
        File.WriteAllText(Path.Combine(root, "tools", "source_version.txt"), "b5724b2" + new string('0', 33) + " 2026-11-26\n");
        var sb = new StringBuilder();
        for (int i = 0; i < 40000; i++) sb.Append("[7/10] Build the game line ").Append(i).Append(" in ").Append(root).Append('\n');
        sb.Append("ERROR: the build stopped (code 2); its last lines:\n  error: arcropolis/src/lib.c:12: 'x' undeclared\n");
        File.WriteAllText(Path.Combine(root, "install.log"), sb.ToString());
        File.WriteAllText(Path.Combine(root, "build", "logs", "installer_window.log"), "10:00:00  === install\n10:00:01  ERROR: The build stopped\n");
        File.WriteAllText(Path.Combine(root, "build", "play.log"), "[gpu] Vulkan device: Made Up GPU 9000\n");
        return root;
    }

    static void Payload(Action<bool, string> check, string root)
    {
        Install(root);
        var f = new SetupFailure
        {
            Kind = "build_error", Where = "install_opensmash.ps1 (install), step Build the game", Root = root, ExitCode = 1,
            Error = "The build stopped (code 2). " + Path.Combine(root, "build") + "\nsecond line", Output = string.Join("\n", Enumerable.Range(0, 30000).Select(i => "output line " + i)),
        };
        var (code, json) = SetupReport.Build(f, new DateTime(2026, 11, 28, 12, 0, 0, DateTimeKind.Utc));
        check(Schema(json) == "", "payload: logs-send's schema takes it: " + Schema(json));
        using var doc = JsonDocument.Parse(json);
        var b = doc.RootElement;
        string text = b.GetProperty("text").GetString();
        check(b.GetProperty("product").GetString() == "opensmash" && b.GetProperty("kind").GetString() == "build_error", "payload: product and kind");
        check(b.GetProperty("exception").GetString().StartsWith("[" + code + "] install_opensmash.ps1 (install), step Build the game: The build stopped (code 2)."), "payload: exception = code, where, first line: " + b.GetProperty("exception").GetString());
        check(b.GetProperty("version").GetString() == "0.5.0-test" && b.GetProperty("commit").GetString() == "b5724b2" + new string('0', 33), "payload: version and commit");
        check(b.GetProperty("windows").GetString().Length > 0 && b.GetProperty("gpu").GetString() == "Made Up GPU 9000", "payload: OS and GPU");
        check(Encoding.UTF8.GetByteCount(text) <= SetupReport.MaxText, "payload: the text within its cap (" + Encoding.UTF8.GetByteCount(text) + ")");
        check(text.Contains("code:      " + code) && text.Contains("exit code: 1 (0x00000001)") && text.Contains("==================== install.log") && text.Contains("installer_window.log"),
              "payload: header and the logs");
        check(text.Contains("'x' undeclared") && text.Contains("output line 29999") && !text.Contains("output line 0\n"), "payload: the logs' and the output's ends (a compile error is in it)");
        check(!text.Contains(root) && !json.Contains(Environment.UserName + "\\"), "payload: no folder of the install, no user name");
        check(!b.TryGetProperty("note", out _), "payload: no note (the automatic reports have none)");
        var none = new SetupFailure { Kind = "install_error", Where = "first start", Error = "HttpRequestException: no such host" };
        var (_, firstJson) = SetupReport.Build(none);
        using var first = JsonDocument.Parse(firstJson);
        check(Schema(firstJson) == "" && first.RootElement.GetProperty("commit").ValueKind == JsonValueKind.Null, "payload: before an install (no folder): no commit, still valid");
        bool refused = false;
        try { SetupReport.Build(new SetupFailure { Kind = "crash", Error = "x" }); } catch (ArgumentException) { refused = true; }
        check(refused, "payload: only the setup kinds");
        check(Schema(json.Replace("\"opensmash\"", "\"other\"")) == "unknown product" && Schema(json.Replace("\"build_error\"", "\"crash\"")) == "unknown kind",
              "schema: the C# copy refuses what the server refuses");
    }

    static void Limits(Action<bool, string> check, string root)
    {
        string file = Path.Combine(root, "sent", "setup_reports_sent.txt");
        var now = new DateTime(2026, 11, 28, 12, 0, 0, DateTimeKind.Utc);
        var l = new SetupReport.Limiter(file);
        check(l.Take("AAAA0001", now) == "" && l.Take("AAAA0002", now) == "" && l.Take("AAAA0003", now) == "", "limit: 3 a run");
        check(l.Take("AAAA0004", now) == "limit" && l.Taken == 3, "limit: the 4th waits for the next run");
        l.Remember("AAAA0001", now);
        var next = new SetupReport.Limiter(file);
        check(next.Take("AAAA0001", now.AddHours(23)) == "duplicate", "dedup: the same code within 24 h, also in the next run");
        check(next.Take("aaaa0001", now.AddHours(23)) == "duplicate", "dedup: the code's case does not matter");
        check(next.Take("AAAA0001", now.AddHours(25)) == "", "dedup: after 24 h it may go again");
        next.Remember("BBBB0001", now.AddHours(25));
        var lines = File.ReadAllLines(file);
        check(lines.Length == 1 && lines[0].StartsWith("BBBB0001 "), "dedup: codes older than 24 h are dropped from the file: " + string.Join("|", lines));
        File.WriteAllText(file, "garbage\nZZZZ 2026\n");
        check(new SetupReport.Limiter(file).Take("CCCC0001", now) == "", "dedup: a damaged file is no error");
    }

    static async Task Sending(Action<bool, string> check, string root)
    {
        var service = new AccountService { Url = "https://made-up.supabase.test", Key = "sb_publishable_made_up_key" };
        var server = new FakeServer();
        using var http = new HttpClient(server);
        var now = new DateTime(2026, 11, 28, 12, 0, 0, DateTimeKind.Utc);
        string file = Path.Combine(root, "sent2", "sent.txt");
        SetupFailure F(string error) => new SetupFailure { Kind = "install_error", Where = "test", Error = error, Root = root };
        var logs = new List<string>();
        var l = new SetupReport.Limiter(file);
        var r = await SetupReport.SendAsync(F("There is no prod.keys here."), logs.Add, http, l, service, now);
        check(r.Sent && r.ServerCode == "0F1E2D3C" && server.Calls.Count == 1, "send: 200 is sent: " + r.Message + " " + r.Error);
        var (rq, body) = server.Calls[0];
        check(rq.Method == HttpMethod.Post && rq.RequestUri.ToString() == "https://made-up.supabase.test/functions/v1/logs-send", "send: logs-send, POST");
        check(rq.Headers.GetValues("apikey").Single() == "sb_publishable_made_up_key" && rq.Headers.Authorization == null, "send: the publishable key, no session (works signed out)");
        check(rq.Content.Headers.ContentType.MediaType == "application/json" && Schema(body) == "", "send: JSON that logs-send takes");
        r = await SetupReport.SendAsync(F("There is no prod.keys here."), logs.Add, http, l, service, now.AddMinutes(5));
        check(r.Duplicate && server.Calls.Count == 1, "send: the same error again within 24 h: not sent");
        server.Status = HttpStatusCode.Conflict; server.Answer = "{\"error\":\"duplicate\"}";
        r = await SetupReport.SendAsync(F("another error"), logs.Add, http, l, service, now);
        check(r.Duplicate && server.Calls.Count == 2, "send: 409 from the server is a duplicate");
        server.Status = HttpStatusCode.TooManyRequests; server.Answer = "{\"error\":\"too many\"}";
        r = await SetupReport.SendAsync(F("a third error"), logs.Add, http, l, service, now);
        check(!r.Sent && r.Error.Contains("too many") && r.Message.Contains("could not be sent"), "send: 429 said");
        r = await SetupReport.SendAsync(F("a fourth error"), logs.Add, http, l, service, now);
        check(r.Limited && server.Calls.Count == 3, "send: the run's 3 are used up: no 4th request");
        server.Status = HttpStatusCode.OK;
        Environment.SetEnvironmentVariable(SetupReport.Variable, "0");
        r = await SetupReport.SendAsync(F("off"), logs.Add, http, new SetupReport.Limiter(null), service, now);
        check(r.Off && server.Calls.Count == 3 && r.Code.Length == 8, "send: off sends nothing, the code is still shown");
        Environment.SetEnvironmentVariable(SetupReport.Variable, null);
        r = await SetupReport.SendAsync(F("no service"), logs.Add, http, new SetupReport.Limiter(null), new AccountService(), now);
        check(!r.Sent && r.Error.Contains("no accounts service") && server.Calls.Count == 3, "send: a build without the accounts service");
        string testFile = Path.Combine(root, "test", "request.json");
        Environment.SetEnvironmentVariable(SetupReport.TestVariable, testFile);
        r = await SetupReport.SendAsync(F("test mode"), logs.Add, http, new SetupReport.Limiter(null), service, now);
        Environment.SetEnvironmentVariable(SetupReport.TestVariable, null);
        check(r.Test && File.Exists(testFile) && Schema(File.ReadAllText(testFile)) == "" && server.Calls.Count == 3, "send: test mode writes the request, sends nothing");
        check(logs.Any(s => s.Contains("sent: report 0F1E2D3C")) && logs.Any(s => s.Contains("test")), "send: every outcome logged");
    }

    static void Stale(Action<bool, string> check, string root)
    {
        string a = "cab62fe" + new string('1', 33), b = "b5724b2" + new string('2', 33), c = "1234567" + new string('3', 33);
        var v054 = SemVer.Parse("0.5.4");
        var f = StaleGame.Check(v054, v054, a, b, null);
        check(f?.Reason == "sources_behind" && f.Message.Contains("Press Update"), "stale: launcher 0.5.4 with 0.5.3's sources");
        f = StaleGame.Check(SemVer.Parse("0.5.1"), v054, a, a, null);
        check(f?.Reason == "launcher_older" && f.Message.Contains("older"), "stale: launcher 0.5.1 with 0.5.4's sources");
        check(StaleGame.Check(v054, v054, a, a, null) == null, "stale: launcher and sources of the same release");
        check(StaleGame.Check(SemVer.Parse("0.0.0-dev"), v054, a, b, null) == null, "stale: a local launcher is never compared with a release");
        check(StaleGame.Check(v054, null, null, b, null) == null, "stale: no release known, no record: nothing said");
        var rec = new Dictionary<string, string> { ["commit"] = b };
        f = StaleGame.Check(v054, null, null, a, rec);
        check(f?.Reason == "not_rebuilt" && f.Message.Contains("not rebuilt"), "stale: the build's record says other sources");
        rec["current"] = c + " " + a;
        check(StaleGame.Check(v054, null, null, a, rec) == null, "stale: a build found current for these sources is not stale");
        Directory.CreateDirectory(Path.Combine(root, "build"));
        File.WriteAllText(Path.Combine(root, "build", "build_commit.txt"), "# made by the build\ncommit " + b + "\nexe_at 1234\ncurrent " + c + "\n");
        check(StaleGame.RecordedCommit(root) == b && StaleGame.Record(root)["current"] == c, "stale: build_commit.txt read");
        File.WriteAllText(Path.Combine(root, "tools", "source_version.txt"), a + " 2026-11-27\n");
        check(StaleGame.Check(root, null)?.Reason == "not_rebuilt", "stale: an install checked from its files");
        Environment.SetEnvironmentVariable(StaleGame.Variable, "0");
        check(StaleGame.Check(root, null) == null, "stale: OPENSMASH_STALE_CHECK=0, no check");
        Environment.SetEnvironmentVariable(StaleGame.Variable, null);
        var sf = StaleGame.Failure(root, StaleGame.Check(root, null));
        var (_, json) = SetupReport.Build(sf);
        check(sf.Kind == "stale_game" && Schema(json) == "", "stale: its report is valid");
        File.Delete(Path.Combine(root, "build", "build_commit.txt"));
    }

    static void Start(Action<bool, string> check, string root)
    {
        string log = Path.Combine(root, "build", "start.log");
        File.WriteAllText(log, "[boot] cannot use the game library " + Path.Combine(root, "build", "ssbu_game.dll") + ": it could not be loaded (error 126)\n" +
                               "This OpenSmash's game library (ssbu_game.dll) is missing, or was built for another version of OpenSmash.\n");
        var f = StaleGame.StartFailure(root, log, 1);
        check(f?.Kind == "game_start_error" && f.Where.Contains("LoadLibrary error 126") && f.Error.Contains("(error 126)"), "start: the game library not loaded (#599)");
        var (_, json) = SetupReport.Build(f);
        check(Schema(json) == "" && !json.Contains(root.Replace("\\", "\\\\")), "start: its report is valid and clean");
        File.WriteAllText(log, "");
        f = StaleGame.StartFailure(root, log, unchecked((int)0xC0000135));
        check(f?.Kind == "game_start_error" && f.Error.Contains("before writing a line") && f.Error.Contains("C0000135"), "start: ended before writing a line");
        check(StaleGame.StartFailure(root, Path.Combine(root, "none.log"), 3)?.Kind == "game_start_error", "start: no log at all");
        File.WriteAllText(log, "[gfx] window ready\n[error] something later\n");
        check(StaleGame.StartFailure(root, log, 1) == null, "start: a game that wrote its log is the crash reports' business");
        File.WriteAllText(log, "");
        check(StaleGame.StartFailure(root, log, 0) == null, "start: exit 0 is no failure");
    }
}
