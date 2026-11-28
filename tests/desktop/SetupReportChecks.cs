// (v0.5.5) The shared launcher's setup error reports (MainWindow.SetupReports.cs, Visuals/SetupReportNotes.cs): the failed step
// an install's report is made from (install_error or build_error), the code shown where the error is, a game that could not
// start, a game not rebuilt after an update, the expected errors of LauncherCrash. Nothing is sent: OPENSMASH_SETUP_REPORT_TEST
// writes the request to a file, which is checked against logs-send's schema (its fields and kinds).
using System.Net.Http;
using System.Reflection;
using System.Text.Json;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Core;

static class SetupReportChecks
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);Console.WriteLine("PASS "+message);}
    static void Until(Func<bool> condition,string message){var until=DateTime.UtcNow.AddSeconds(30);while(!condition()&&DateTime.UtcNow<until){Dispatcher.UIThread.RunJobs();Thread.Sleep(20);}Check(condition(),message);}
    static readonly string[] Kinds=["install_error","update_error","build_error","launcher_crash","stale_game","game_start_error"];

    static JsonElement Request(string file)
    {
        using var doc=JsonDocument.Parse(File.ReadAllText(file));var b=doc.RootElement.Clone();
        Check(b.GetProperty("product").GetString()=="opensmash"&&Kinds.Contains(b.GetProperty("kind").GetString())&&b.GetProperty("text").GetString()!.Trim().Length>0
              &&b.GetProperty("exception").GetString()!.Length<=400&&b.GetProperty("windows").GetString()!.Length<=100,"setup report: the request has logs-send's fields");
        return b;
    }

    public static void Run(string temporary)
    {
        string repo=Path.Combine(temporary,"setup-reports");Directory.CreateDirectory(Path.Combine(repo,"tools"));Directory.CreateDirectory(Path.Combine(repo,"runtime"));
        File.WriteAllText(Path.Combine(repo,"tools","os_mods.py"),"");
        File.WriteAllText(Path.Combine(repo,"tools","source_version.txt"),"cab62fe"+new string('1',33)+" 2026-11-27\n");
        string request=Path.Combine(temporary,"setup-report-request.json");
        string? was=Environment.GetEnvironmentVariable(SetupReport.TestVariable);
        Environment.SetEnvironmentVariable(SetupReport.TestVariable,request);
        SetupReport.UseLimiter(new SetupReport.Limiter(null,100));   // (more than a run's 3 here; the limits are tests/core's)
        try
        {
            Check(LauncherCrash.Expected(new HttpRequestException("x"))&&LauncherCrash.Expected(new AggregateException(new OperationCanceledException()))&&!LauncherCrash.Expected(new NullReferenceException()),
                  "launcher crash: a lost connection or a cancel is not reported, the rest is");
            var window=new MainWindow(["--project",repo],repo,enableBackgroundActions:false);
            var surface=window.Surface;var flags=BindingFlags.Instance|BindingFlags.NonPublic;
            // an install whose build step failed: build_error, its code under the error
            surface.BeginInstall();
            surface.ReceiveInstallLine("##ssbu {\"ev\":\"plan\",\"steps\":[{\"id\":\"game\",\"title\":\"Your game files\",\"est_s\":10},{\"id\":\"build\",\"title\":\"Build the game\",\"est_s\":100}]}");
            surface.ReceiveInstallLine("##ssbu {\"ev\":\"step\",\"id\":\"game\",\"state\":\"done\"}");
            surface.ReceiveInstallLine("##ssbu {\"ev\":\"step\",\"id\":\"build\",\"state\":\"start\"}");
            surface.ReceiveInstallLine("error: arcropolis/src/lib.c:12: 'x' undeclared");
            surface.ReceiveInstallLine("##ssbu {\"ev\":\"step\",\"id\":\"build\",\"state\":\"fail\",\"msg\":\"the build stopped (code 2)\"}");
            surface.ReceiveInstallLine("##ssbu {\"ev\":\"error\",\"msg\":\"The game could not be built.\",\"hint\":\"See the log.\"}");
            surface.EndInstall(1);
            var (error,step)=surface.InstallFailureInfo();
            Check(error=="The game could not be built."&&step=="build Build the game"&&SetupReport.KindFor("install",step,error)=="build_error","setup report: the install's failed step makes it a build_error");
            var project=new Project(repo,Project.CurrentPlatform);
            typeof(MainWindow).GetMethod("ReportInstallStop",flags)!.Invoke(window,[project,1]);
            string hint()=>(string)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_errHint",flags)!.GetValue(surface)!;
            Until(()=>File.Exists(request)&&hint().Contains("test mode"),"setup report: the install's report written, its line under the error: "+hint());
            var b=Request(request);
            Check(b.GetProperty("kind").GetString()=="build_error"&&b.GetProperty("text").GetString()!.Contains("'x' undeclared")&&b.GetProperty("exception").GetString()!.Contains("Build the game"),
                  "setup report: the compile error's line is in the report");
            Check(!b.GetProperty("text").GetString()!.Contains(repo),"setup report: no install folder in it");
            File.Delete(request);
            // the line shown at once replaced by the result in an open message
            surface.Alert("UPDATE DID NOT FINISH","Update step stopped (exit 1).\n\nError code ABCD1234 — sending it to the developers…");
            surface.ShowReportLine("Error code ABCD1234 — sending it to the developers…","Error code ABCD1234 — this was sent to the developers.");
            var modal=typeof(OpenSmash.Visuals.ClassicSurface).GetField("_modal",flags)!.GetValue(surface)!;
            string body=(string)modal.GetType().GetField("Body")!.GetValue(modal)!;
            Check(body.EndsWith("this was sent to the developers.")&&!body.Contains("sending"),"setup report: the message's line becomes the result");
            surface.Activate("modal:yes");
            // an update that stopped: update_error with the step's own error line
            string line=(string)typeof(MainWindow).GetMethod("ReportUpdateStop",flags)!.Invoke(window,[project,new InvalidOperationException("Update step stopped (exit 1).\nRemove-Item : Access to the path is denied.\nnothing else"),"sources"])!;
            Until(()=>File.Exists(request),"setup report: the update's report written");
            b=Request(request);
            Check(line.StartsWith("Error code ")&&b.GetProperty("kind").GetString()=="update_error"&&b.GetProperty("exception").GetString()!.Contains("Access to the path is denied"),"setup report: an update stopped by access denied");
            File.Delete(request);
            // a game that could not use its game library
            string log=Path.Combine(repo,"build","opensmash-play.log");Directory.CreateDirectory(Path.GetDirectoryName(log)!);
            File.WriteAllText(log,"[boot] cannot use the game library "+Path.Combine(repo,"build","ssbu_game.dll")+": it could not be loaded (error 126)\nThis OpenSmash's game library is missing.\n");
            line=(string)typeof(MainWindow).GetMethod("ReportGameStart",flags)!.Invoke(window,[repo,1,log])!;
            Until(()=>File.Exists(request),"setup report: the game start's report written");
            b=Request(request);
            Check(line.StartsWith("Error code ")&&b.GetProperty("kind").GetString()=="game_start_error"&&b.GetProperty("exception").GetString()!.Contains("LoadLibrary error 126"),"setup report: the game library's error 126");
            File.Delete(request);
            File.WriteAllText(log,"[gfx] window ready\n");
            string none=(string)typeof(MainWindow).GetMethod("ReportGameStart",flags)!.Invoke(window,[repo,1,log])!;
            Check(none.Length==0,"setup report: a game that wrote its log is not a start error");
            // a game not rebuilt after an update (the build's record names other sources): said, reported, Play stopped once
            File.WriteAllText(Path.Combine(repo,"build","build_commit.txt"),"commit b5724b2"+new string('2',33)+"\n");
            var task=(Task<bool>)typeof(MainWindow).GetMethod("CheckStaleGame",flags)!.Invoke(window,[project])!;
            Until(()=>task.IsCompleted,"stale game: the check ends");
            Check(task.Result&&surface.HasPrompt,"stale game: told before Play");
            Until(()=>File.Exists(request),"stale game: its report written");
            Check(Request(request).GetProperty("kind").GetString()=="stale_game","stale game: reported as stale_game");
            surface.Activate("modal:yes");
            var again=(Task<bool>)typeof(MainWindow).GetMethod("CheckStaleGame",flags)!.Invoke(window,[project])!;
            Until(()=>again.IsCompleted,"stale game: the second check ends");
            Check(!again.Result,"stale game: said once a run, the next Play starts the game as it is");
            File.Delete(request);
            window.Close();
        }
        finally{Environment.SetEnvironmentVariable(SetupReport.TestVariable,was);}
    }
}
