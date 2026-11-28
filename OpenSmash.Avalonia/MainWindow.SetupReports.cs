// (v0.5.5) The shared launcher's setup error reports (OpenSmash.Core SetupReport.cs): the first download, the install and its
// build, an update (its steps: the sources, Python's packages, the game's rebuild - the end of the step's output, where a
// standard plugin's or ARCropolis' compile error is), the launcher's own crashes (LauncherCrash below), and at Play a game not
// rebuilt after an update (StaleGame.cs) or one that could not start (its game library did not load, or it wrote nothing).
// The error code goes with the message the player already gets, and what became of the report replaces it when known.
// OPENSMASH_SETUP_REPORTS=0: none of it; OPENSMASH_STALE_CHECK=0: no stale check.
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using Avalonia.Threading;
using OpenSmash.Core;

namespace OpenSmash;

public sealed partial class MainWindow
{
    bool _staleSaid;
    Task<Release?>? _staleRelease;

    static void ReportLog(string? root,string s)=>Updater.Log(root,s);

    /// <summary>f sent in the background; show gets the first line (its code) at once, and later (on the window's thread) the
    /// first line again with the result's</summary>
    string ReportSetup(SetupFailure f,Action<string?,string>? show=null)
    {
        if(!SetupReport.SwitchOn())return "";   // (off: nothing sent, nothing said, as before)
        f.Root??=_project?.Root;
        string code=SetupReport.Code(f.Kind,f.Error,f.Root);
        string first=SetupReport.Enabled(f.Root)?"Error code "+code+" — sending it to the developers…":new SetupReportResult{Code=code,Off=true}.Message;
        try{show?.Invoke(null,first);}catch(InvalidOperationException){}
        if(!SetupReport.Enabled(f.Root))return first;
        string? root=f.Root;
        _=Task.Run(async()=>
        {
            var r=await SetupReport.SendAsync(f,s=>ReportLog(root,s));
            if(show!=null&&!_closed)Dispatcher.UIThread.Post(()=>{try{show(first,r.Message);}catch(InvalidOperationException){}});
        });
        return first;
    }

    /// <summary>(Install) the setup stopped with an error, not by the player: install_error or build_error by its failed step</summary>
    void ReportInstallStop(Project p,int exit)
    {
        var (error,step)=Surface.InstallFailureInfo();
        string tail=Surface.InstallLogTail(400);
        if(error.Length==0||error.StartsWith("Setup stopped (exit",StringComparison.Ordinal))
        {   // (only "Setup stopped (exit N)": the log's last error line first - the code is made of the first line)
            string said=tail.Split('\n').Select(l=>l.Trim()).LastOrDefault(l=>System.Text.RegularExpressions.Regex.IsMatch(l,@"\b(error|failed|fatal|cannot|could not|denied|in use)\b",System.Text.RegularExpressions.RegexOptions.IgnoreCase))??"";
            if(said.Length>0)error=said+(error.Length>0?"\n"+error:"");
        }
        var f=new SetupFailure
        {
            Kind=SetupReport.KindFor("install",step,error),
            Where=(p.Platform==DesktopPlatform.Windows?"install_opensmash.ps1":"install.sh")+(step.Length>0?", step "+step:""),
            Error=error.Length>0?error:"Setup stopped (exit "+exit+").",ExitCode=exit,Root=p.Root,
            Output=tail,Logs=[p.PathIn("build","logs","installer_window.log"),p.PathIn("build","logs","shader_window.log")],
        };
        ReportSetup(f,(_,line)=>Surface.ShowInstallReportLine(line));
    }

    /// <summary>(ApplyUpdate) the update stopped: update_error, or build_error when the game's rebuild did</summary>
    string ReportUpdateStop(Project p,Exception error,string stage)
    {
        string message=error.Message.Replace("\r","");
        string[] lines=message.Split('\n',StringSplitOptions.RemoveEmptyEntries|StringSplitOptions.TrimEntries);
        // ("Update step stopped (exit 1)." alone says nothing: the step's last error line with it)
        string said=lines.Skip(1).LastOrDefault(l=>System.Text.RegularExpressions.Regex.IsMatch(l,@"\b(error|failed|fatal|cannot|could not|denied|in use)\b",System.Text.RegularExpressions.RegexOptions.IgnoreCase))??"";
        var f=new SetupFailure
        {
            Kind=stage=="rebuild"?"build_error":SetupReport.KindFor("update",null,said),
            Where="update: "+stage,Error=SetupReport.Describe(error)+(said.Length>0?" — "+said:""),Root=p.Root,
            Output=message,Detail=error.ToString(),
            Logs=[p.PathIn("build","logs","source_update.log"),p.PathIn("build","logs","update_game_build.log"),p.PathIn("build","logs","python_setup.log")],
        };
        return ReportSetup(f,(before,line)=>Surface.ShowReportLine(before,line));
    }

    /// <summary>(Play) the check, once a run: told and reported when the game is not the launcher's or was not rebuilt; true:
    /// found (this Play stops, the next one starts the game as it is)</summary>
    async Task<bool> CheckStaleGame(Project p)
    {
        if(_staleSaid||!StaleGame.On||Updater.DevCopy(p.Root))return false;
        Release? release=null;
        try
        {
            // (the window's background actions off - the tests - : no network, the build's record only)
            if(_backgroundActions)_staleRelease??=Task.Run(async()=>{try{using var cts=new CancellationTokenSource(TimeSpan.FromSeconds(8));return await Releases.LatestAsync(Updater.Channel(p.Root),cts.Token);}catch(Exception e) when(e is HttpRequestException or TaskCanceledException or OperationCanceledException or System.Text.Json.JsonException or IOException){return null;}});
            // (no waiting at Play for a slow network: the release of a check started before, else none this time)
            if(_staleRelease!=null)release=await Task.WhenAny(_staleRelease,Task.Delay(1500))==_staleRelease?_staleRelease.Result:null;
        }
        catch(Exception e) when(e is InvalidOperationException or IOException){}
        StaleGame.Finding? found;
        try{found=StaleGame.Check(p.Root,release);}catch(Exception e) when(e is IOException or UnauthorizedAccessException){return false;}
        if(found==null)return false;
        _staleSaid=true;ReportLog(p.Root,"stale game: "+found.Detail);
        string line=ReportSetup(StaleGame.Failure(p.Root,found),(before,now)=>Surface.ShowReportLine(before,now));
        Surface.Alert("UPDATE NOT FINISHED",found.Message+" Press Play again to start it as it is."+(line.Length>0?"\n\n"+line:""));
        return true;
    }

    /// <summary>(after a game that ended with an error) a game_start_error when it could not use its game library or wrote
    /// nothing; the line for the message ("" when neither)</summary>
    string ReportGameStart(string? root,int exit,string log)
    {
        var f=StaleGame.StartFailure(root??_project?.Root??"",log,exit);
        return f==null?"":ReportSetup(f);
    }
}

/// <summary>(v0.5.5) the shared launcher's own crashes: an exception nobody caught on the window's thread (kept open, the error
/// shown), on another thread (the launcher closes), in a task nobody waited for. Written to build/launcher_crash.log and sent
/// as launcher_crash. OPENSMASH_SETUP_REPORTS=0: not installed, as before. A cancelled wait or a lost connection is written,
/// never reported.</summary>
static class LauncherCrash
{
    public static volatile string? Root;
    static int s_inside,s_fatal,s_uiErrors;

    static string LogPath=>Root!=null?Path.Combine(Root,"build","launcher_crash.log"):Path.Combine(AppContext.BaseDirectory,"launcher_crash.log");

    internal static bool Expected(Exception ex)
    {
        var b=ex is AggregateException a&&a.InnerExceptions.Count==1?a.InnerExceptions[0]:ex;
        return b is OperationCanceledException or HttpRequestException or SocketException or ObjectDisposedException;
    }

    public static void Install()
    {
        if(!SetupReport.SwitchOn())return;
        AppDomain.CurrentDomain.UnhandledException+=(_,e)=>
        {
            if(Interlocked.Exchange(ref s_fatal,1)==1)return;
            var ex=e.ExceptionObject as Exception??new Exception(Convert.ToString(e.ExceptionObject));
            Write(ex,"background thread"+(e.IsTerminating?", the launcher closes":""));
            try{Task.Run(()=>SetupReport.SendAsync(Failure(ex,"background thread"),s=>Updater.Log(Root,s))).Wait(8000);}catch(AggregateException){}
        };
        TaskScheduler.UnobservedTaskException+=(_,e)=>
        {
            try{e.SetObserved();}catch(InvalidOperationException){}
            var ex=e.Exception?.InnerExceptions.Count==1?e.Exception.InnerExceptions[0]:e.Exception;
            if(ex!=null)Handle(ex,"background task");
        };
        Dispatcher.UIThread.UnhandledException+=(_,e)=>
        {
            if(Interlocked.Increment(ref s_uiErrors)>20){Write(e.Exception,"window thread, 20th error: the launcher closes");return;}
            e.Handled=true;
            Handle(e.Exception,"window thread");
        };
    }

    static SetupFailure Failure(Exception ex,string thread)=>new()
    {
        Kind="launcher_crash",Root=Root,Where="shared launcher, "+thread,Error=SetupReport.Describe(ex.GetBaseException()),Detail=ex.ToString(),Logs=[LogPath],
    };

    static void Handle(Exception ex,string thread)
    {
        if(Interlocked.Exchange(ref s_inside,1)==1)return;
        try
        {
            if(Expected(ex)){Write(ex,thread+", expected (cancelled or no connection): not reported");return;}
            Write(ex,thread);
            _=Task.Run(()=>SetupReport.SendAsync(Failure(ex,thread),s=>Updater.Log(Root,s)));
        }
        catch(Exception inner) when(inner is IOException or InvalidOperationException){}
        finally{Interlocked.Exchange(ref s_inside,0);}
    }

    static void Write(Exception? ex,string thread)
    {
        try
        {
            string path=LogPath;
            var sb=new StringBuilder();
            sb.Append("=== ").Append(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss")).Append("  launcher ").Append(Updater.Current).Append("  ").Append(thread).Append('\n');
            sb.Append(ex?.ToString()??"(no exception)").Append("\n\n");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            if(File.Exists(path)&&new FileInfo(path).Length>512<<10){string old=File.ReadAllText(path);File.WriteAllText(path,old[(old.Length/2)..]);}
            File.AppendAllText(path,sb.ToString());
            Updater.Log(Root,"launcher error ("+thread+"): "+SetupReport.FirstLine(ex?.Message)+", see launcher_crash.log");
        }
        catch(Exception e) when(e is IOException or UnauthorizedAccessException){}
    }
}
