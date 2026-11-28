// (v0.5.5) The Windows launcher's setup error reports (SetupReports.cs, LauncherForm.SetupReports.cs): the setting "Send setup
// error reports" (on by default, config\setup_reports.txt), the most telling line of an update's or a build's output (the
// report's error line and its code), the expected errors that are never reported, the player's first line. Nothing is sent:
// OPENSMASH_SETUP_REPORT_TEST writes the request to a file.
using System.Net.Http;
using System.Reflection;
using OpenSmash.Launcher;

static partial class Tests
{
    static void SetupReportChecks(string repo)
    {
        Directory.CreateDirectory(repo);
        // (more than a run's 3 here and no file of this PC's: the limits are tests/core's)
        OpenSmash.Core.SetupReport.UseLimiter(new OpenSmash.Core.SetupReport.Limiter(null, 100));
        var (form, call, values) = Open(repo);
        using (form)
        {
            call("ReadSettings");
            Check(values["setup_reports"] == "on", "setup reports: on by default");
            var flags = BindingFlags.Instance | BindingFlags.NonPublic;
            var tabs = (System.Collections.IEnumerable)typeof(LauncherForm).GetField("_settingsTabs", flags)!.GetValue(form)!;
            bool listed = tabs.Cast<object>().Any(t => (string)t.GetType().GetField("Name")!.GetValue(t)! == "Online"
                                                       && ((string[])t.GetType().GetField("SettingKeys")!.GetValue(t)!).Contains("setup_reports"));
            Check(listed, "setup reports: the setting is on the Online tab");
            values["setup_reports"] = "off";
            ((HashSet<string>)typeof(LauncherForm).GetField("_changed", flags)!.GetValue(form)!).Add("setup_reports");
            call("PersistSettings");
            Check(File.ReadAllText(Path.Combine(repo, "config", "setup_reports.txt")).Trim() == "off" && !OpenSmash.Core.SetupReport.Enabled(repo),
                  "setup reports: off written to config\\setup_reports.txt, and it stops them");
        }
        var (form2, call2, values2) = Open(repo);
        using (form2)
        {
            call2("ReadSettings");
            Check(values2["setup_reports"] == "off", "setup reports: off read back");
        }
        File.Delete(Path.Combine(repo, "config", "setup_reports.txt"));

        string output = "##ssbu {\"ev\":\"step\"}\n[9/10] Build the game\nLOG percent 40\n  compiling plugins\n" +
                        "error: arcropolis/src/lib.c:12: 'x' undeclared\nthe build stopped (code 2); its last lines:\n  done\n";
        Check(LauncherForm.ErrorLine(output) == "the build stopped (code 2); its last lines:", "error line: the last line that says it failed: " + LauncherForm.ErrorLine(output));
        Check(LauncherForm.ErrorLine("one\ntwo\n") == "two" && LauncherForm.ErrorLine("") == "", "error line: else the last line");
        Check(LauncherForm.ErrorLine("Remove-Item : Access to the path is denied.\nok") == "Remove-Item : Access to the path is denied.", "error line: access denied");

        Check(SetupReports.Expected(new HttpRequestException("no host")) && SetupReports.Expected(new OperationCanceledException())
              && SetupReports.Expected(new AggregateException(new TaskCanceledException())), "launcher crash: no connection or a cancel is not reported");
        Check(!SetupReports.Expected(new NullReferenceException()) && !SetupReports.Expected(new IOException("x")), "launcher crash: the rest is");

        var f = new OpenSmash.Core.SetupFailure { Kind = "install_error", Where = "test", Error = "There is no prod.keys here.", Root = repo };
        string first = SetupReports.FirstMessage(f);
        Check(System.Text.RegularExpressions.Regex.IsMatch(first, "^Error code [0-9A-F]{8} — sending it to the developers"), "setup reports: the code shown at once: " + first);
        string was = Environment.GetEnvironmentVariable("OPENSMASH_SETUP_REPORTS") ?? "";
        Environment.SetEnvironmentVariable("OPENSMASH_SETUP_REPORTS", "0");
        Check(SetupReports.FirstMessage(f) == "", "setup reports: OPENSMASH_SETUP_REPORTS=0 says nothing, as before");
        Environment.SetEnvironmentVariable("OPENSMASH_SETUP_REPORTS", was.Length > 0 ? was : null);
        OpenSmash.Core.SetupReport.SetSetting(repo, false);
        Check(SetupReports.FirstMessage(f).Contains("reports are off") && SetupReports.FirstMessage(f).Contains("Error code "), "setup reports: the setting off still shows the code to quote");
        OpenSmash.Core.SetupReport.SetSetting(repo, true);

        // the report itself, through the window's helper, in test mode: what would go out (and nothing goes out)
        string request = Path.Combine(repo, "request.json");
        Environment.SetEnvironmentVariable("OPENSMASH_SETUP_REPORT_TEST", request);
        try
        {
            string? last = null;
            var done = new ManualResetEventSlim();
            using var host = new Form();
            var handle = host.Handle;   // (the result comes back on the window's thread)
            SetupReports.Send(new OpenSmash.Core.SetupFailure { Kind = "build_error", Where = "test", Error = "exit 2: the build stopped", Root = repo }, host, line => { last = line; if (!line.Contains("sending")) done.Set(); });
            var until = DateTime.UtcNow.AddSeconds(20);
            while (!done.IsSet && DateTime.UtcNow < until) { Application.DoEvents(); Thread.Sleep(20); }
            Check(done.IsSet && last!.Contains("test mode") && File.Exists(request) && File.ReadAllText(request).Contains("\"kind\":\"build_error\""),
                  "setup reports: sent in the background, the result on the window's thread: " + last);
        }
        finally { Environment.SetEnvironmentVariable("OPENSMASH_SETUP_REPORT_TEST", null); }
        InstallerStopReported(Path.Combine(repo, "installer"), EventScript, "step Build the game: The game could not be built", "events");
        // the real installer's own way: "[9/10] Build the game" headings, and its catch's "ERROR: ..." line
        InstallerStopReported(Path.Combine(repo, "installer-plain"), new[]
        {
            "Write-Output '[7/10] Your game files'", "Write-Output '[9/10] Build the game'",
            "Write-Output 'error: arcropolis/src/lib.c:12: x undeclared'", "Write-Output ''",
            "Write-Output 'ERROR: The build stopped (code 2): a plugin did not compile.'", "exit 1",
        }, "step Build the game: The build stopped (code 2): a plugin did not compile.", "plain output");
    }

    static readonly string[] EventScript =
        {
            "Write-Output '##ssbu {\"ev\":\"plan\",\"steps\":[{\"id\":\"game\",\"title\":\"Your game files\",\"est_s\":10},{\"id\":\"build\",\"title\":\"Build the game\",\"est_s\":100}]}'",
            "Write-Output '##ssbu {\"ev\":\"step\",\"id\":\"game\",\"state\":\"done\"}'",
            "Write-Output '##ssbu {\"ev\":\"step\",\"id\":\"build\",\"state\":\"start\"}'",
            "Write-Output \"compiling $PSScriptRoot\\..\\dump\\skyline\\arcropolis\"",
            "Write-Output 'error: arcropolis/src/lib.c:12: x undeclared'",
            "Write-Output '##ssbu {\"ev\":\"step\",\"id\":\"build\",\"state\":\"fail\",\"msg\":\"the build stopped (code 2)\"}'",
            "Write-Output '##ssbu {\"ev\":\"error\",\"msg\":\"The game could not be built: a plugin did not compile.\",\"hint\":\"See the log.\"}'",
            "exit 2",
        };

    // The installer window end to end: a made-up tools\install_opensmash.ps1 that stops at the game's build the way the real
    // one does (its events, an ARCropolis compile error, exit 2) run as the window runs it (powershell.exe), read, finished,
    // reported - in test mode: the request is written, not sent - and the code on the status line.
    static void InstallerStopReported(string repo, string[] script, string expected, string how)
    {
        Directory.CreateDirectory(Path.Combine(repo, "tools"));
        File.WriteAllText(Path.Combine(repo, "tools", "source_version.txt"), "cab62fe" + new string('1', 33) + " 2026-11-27\n");
        string stub = Path.Combine(repo, "tools", "install_opensmash.ps1");
        File.WriteAllText(stub, string.Join("\r\n", script));
        string request = Path.Combine(repo, "request.json");
        Environment.SetEnvironmentVariable("OPENSMASH_SETUP_REPORT_TEST", request);
        try
        {
            using var form = new InstallerForm(repo, repo);
            var handle = form.Handle;
            var flags = BindingFlags.Instance | BindingFlags.NonPublic;
            var type = typeof(InstallerForm);
            type.GetMethod("RunJob", flags)!.Invoke(form, new object[] { "install", false, "powershell.exe",
                new List<string> { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", stub } });
            var tick = type.GetMethod("OnTick", flags)!;
            bool Running() => (bool)type.GetField("_running", flags)!.GetValue(form)!;
            string Status() => (string)type.GetField("_status", flags)!.GetValue(form)!;
            var until = DateTime.UtcNow.AddSeconds(60);
            while ((Running() || !Status().Contains("test mode")) && DateTime.UtcNow < until) { tick.Invoke(form, null); Application.DoEvents(); Thread.Sleep(20); }
            Check(!Running() && Status().StartsWith("Stopped. Error code ") && Status().Contains("test mode"), "installer (" + how + "): a stopped build shows its error code: " + Status());
            string json = File.ReadAllText(request);
            using var doc = System.Text.Json.JsonDocument.Parse(json);
            var b = doc.RootElement;
            Check(b.GetProperty("product").GetString() == "opensmash" && b.GetProperty("kind").GetString() == "build_error", "installer (" + how + "): reported as a build_error of OpenSmash");
            Check(b.GetProperty("exception").GetString()!.Contains(expected), "installer (" + how + "): where and the error: " + b.GetProperty("exception").GetString());
            string text = b.GetProperty("text").GetString()!;
            Check(text.Contains("exit code: ") && text.Contains("x undeclared") && text.Contains("installer_window.log"), "installer (" + how + "): exit code, the compile error and the window's log in it");
            Check(!text.Contains(repo) && !json.Contains(repo.Replace("\\", "\\\\")), "installer (" + how + "): the install's folder is not in it");
        }
        finally { Environment.SetEnvironmentVariable("OPENSMASH_SETUP_REPORT_TEST", null); }
    }
}
