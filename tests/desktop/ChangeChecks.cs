using System.Reflection;
using System.Text.Json;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Core;

static class ChangeChecks
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);Console.WriteLine("PASS "+message);}
    public static void Tracker(string root)
    {
        string fixture=Path.Combine(root,"change-tracker");Directory.CreateDirectory(Path.Combine(fixture,"runtime"));Directory.CreateDirectory(Path.Combine(fixture,"tools"));File.WriteAllText(Path.Combine(fixture,"tools","os_mods.py"),"");
        var project=new Project(fixture,Project.CurrentPlatform);var changes=new ModChanges(project);
        string mods=project.PathIn("mods");Directory.CreateDirectory(Path.Combine(mods,"existing"));
        Check(changes.Scan().Length==0,"first scan seeds existing unindexed mods");
        Directory.CreateDirectory(Path.Combine(mods,"new moveset"));Directory.CreateDirectory(Path.Combine(mods,".staging"));
        Check(changes.Scan().SequenceEqual(["new moveset"]),"offline-added mod detected, hidden staging folder ignored");
        File.WriteAllText(Path.Combine(mods,"mods.json"),"{\"mods\":{\"existing\":{},\"new moveset\":{}}}");
        Check(new ModChanges(project).Scan().SequenceEqual(["new moveset"]),"status indexing and restart do not acknowledge an unbuilt mod");
        Directory.CreateDirectory(Path.Combine(mods,"new moveset","prebuilt"));File.WriteAllText(Path.Combine(mods,"new moveset","prebuilt","ignored.nro"),"");
        Check(!changes.HasPlugin("new moveset"),"prebuilt cache is excluded from source-plugin detection");
        File.WriteAllText(Path.Combine(mods,"new moveset","actual.NRO"),"");Check(changes.HasPlugin("new moveset"),"source plugin detection handles uppercase extension");
        changes.Remember(["new moveset"]);Check(new ModChanges(project).Scan().Length==0,"successful operation acknowledgement survives restart");
        Directory.Delete(Path.Combine(mods,"existing"));changes.Scan();Directory.CreateDirectory(Path.Combine(mods,"existing"));
        Check(changes.Scan().SequenceEqual(["existing"]),"removed then externally re-added mod is detected again");
        string first=project.PathIn("first-indexed");Directory.CreateDirectory(Path.Combine(first,"runtime"));Directory.CreateDirectory(Path.Combine(first,"tools"));File.WriteAllText(Path.Combine(first,"tools","os_mods.py"),"");Directory.CreateDirectory(Path.Combine(first,"mods","old"));Directory.CreateDirectory(Path.Combine(first,"mods","dropped"));File.WriteAllText(Path.Combine(first,"mods","mods.json"),"{\"mods\":{\"old\":{}}}");
        Check(new ModChanges(new Project(first,Project.CurrentPlatform)).Scan().SequenceEqual(["dropped"]),"first shared-launcher scan detects additions absent from existing index");
    }
    static Task Invoke(MainWindow window,string method,params object[] arguments)=>(Task)typeof(MainWindow).GetMethod(method,BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,arguments)!;
    static void PumpUntil(Func<bool> done)
    {
        var deadline=DateTime.UtcNow.AddSeconds(20);
        while(!done()){Dispatcher.UIThread.RunJobs();if(DateTime.UtcNow>deadline)throw new TimeoutException("Change-check UI timed out");Thread.Sleep(10);}
        Dispatcher.UIThread.RunJobs();
    }
    static void Finish(Task task){PumpUntil(()=>task.IsCompleted);task.GetAwaiter().GetResult();}
    public static void Controller(string root)
    {
        string fixture=Path.Combine(root,"change-controller");Directory.CreateDirectory(Path.Combine(fixture,"runtime"));Directory.CreateDirectory(Path.Combine(fixture,"tools"));
        File.WriteAllText(Path.Combine(fixture,"tools","os_mods.py"),"""
import json,sys,pathlib,time
root=pathlib.Path(__file__).resolve().parent.parent
cmd=sys.argv[1]
with (root/'calls.txt').open('a') as f:f.write(cmd+'\n')
if cmd=='status':
 if (root/'hold-status').exists():
  (root/'status-started').touch()
  while (root/'hold-status').exists():time.sleep(.01)
 print((root/'status.json').read_text())
elif cmd=='enable' and (root/'fail-enable').exists():print('RESULT '+json.dumps({'ok':False,'error':'synthetic failure'}))
else:
 print('RESULT '+json.dumps({'ok':True}))
 if not (root/'retain-status').exists():(root/'status.json').write_text('{"mods":[]}')
""");
        var p=new Project(fixture,Project.CurrentPlatform);Directory.CreateDirectory(Path.GetDirectoryName(p.GameExecutable)!);File.WriteAllText(p.GameExecutable,"fixture");Directory.CreateDirectory(p.PathIn("mods"));File.WriteAllText(p.PathIn("mods","mods.json"),"{\"mods\":{}}");
        var changes=new ModChanges(p);changes.Scan();
        var window=new MainWindow([],fixture,enableBackgroundActions:false);var surface=window.Surface;
        void Status(string build="",string refresh="",string vanilla="")=>File.WriteAllText(p.PathIn("status.json"),JsonSerializer.Serialize(new {mods=Array.Empty<object>(),needs_build=build,needs_refresh=refresh,needs_build_vanilla=vanilla}));
        File.WriteAllText(p.PathIn("retain-status"),"");
        Status(refresh:"newly imported plugin");
        Finish(Invoke(window,"ModCommand","enable",new[]{"imported"},true));
        Status(refresh:"newly imported plugin");
        Finish(Invoke(window,"CheckExternalChanges",false,false));
        Check(!surface.HasPrompt,"launcher import does not immediately show an update warning");
        File.Delete(p.PathIn("retain-status"));
        Status(refresh:"plugin observed while choosing a folder");
        var choosing=Invoke(window,"AddLocalMod");
        Check(surface.HasPrompt&&!choosing.IsCompleted,"local import owns the flow before opening the folder picker");
        Finish(Invoke(window,"CheckExternalChanges",false,true));
        Check(!(bool)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_pluginPrompt",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!,"folder-picker focus checks cannot open a plugin warning");
        surface.Activate("modal:no");Finish(choosing);
        Check(!(bool)typeof(MainWindow).GetField("_addingLocalMod",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(window)!,"cancelled folder selection releases the import guard");
        File.WriteAllText(p.PathIn("hold-status"),"");
        var racing=Invoke(window,"CheckExternalChanges",false,false);
        PumpUntil(()=>File.Exists(p.PathIn("status-started")));
        File.WriteAllText(p.PathIn("retain-status"),"");
        var importing=Invoke(window,"ModCommand","enable",new[]{"linked-import"},true);
        File.Delete(p.PathIn("hold-status"));
        PumpUntil(()=>racing.IsCompleted||surface.HasPrompt);
        Check(!surface.HasPrompt,"an in-flight pre-import status check cannot show an update prompt");
        Finish(racing);Finish(importing);
        Finish(Invoke(window,"CheckExternalChanges",false,false));
        Check(!surface.HasPrompt,"focus after import completion keeps the imported plugin warning acknowledged");
        File.Delete(p.PathIn("retain-status"));
        Status("the plugin example isn't in the game","DLL also changed");
        var check=Invoke(window,"CheckExternalChanges",false,true);PumpUntil(()=>surface.HasPrompt||check.IsCompleted);
        Check(surface.PluginBuildReason.Length>0&&surface.PluginRefreshReason.Length>0&&surface.HasPrompt,"external check consumes both backend readiness reasons");
        surface.Activate("plugin_prompt:build");Finish(check);
        var calls=File.ReadAllLines(p.PathIn("calls.txt"));Check(calls.Contains("build")&&!calls.Contains("refresh"),"game-link reason takes precedence over DLL refresh");
        Status("the plugin queued isn't in the game");
        check=Invoke(window,"CheckExternalChanges",false,true);
        Finish(Invoke(window,"Action","play"));
        PumpUntil(()=>surface.HasPrompt||check.IsCompleted);surface.Activate("plugin_prompt:cancel");Finish(check);
        Check(!surface.GameRunning&&!surface.HasPrompt,"Play queued during discovery is cancelled with the readiness prompt");
        File.WriteAllText(p.PathIn("calls.txt"),"");Status(refresh:"plugin DLL stale");
        check=Invoke(window,"CheckExternalChanges",false,true);PumpUntil(()=>surface.HasPrompt||check.IsCompleted);surface.Activate("plugin_prompt:build");Finish(check);
        calls=File.ReadAllLines(p.PathIn("calls.txt"));Check(calls.Contains("refresh")&&!calls.Contains("build"),"DLL-only change refreshes plugins without rebuilding game");
        Status(refresh:"plugin DLL stale");check=Invoke(window,"CheckExternalChanges",false,true);PumpUntil(()=>surface.HasPrompt||check.IsCompleted);surface.Activate("plugin_prompt:cancel");Finish(check);
        check=Invoke(window,"CheckExternalChanges",false,false);Finish(check);Check(!surface.HasPrompt,"cancelled prompt is not repeated on every focus event");
        var playCheck=(Task<bool>)typeof(MainWindow).GetMethod("PrepareExternalChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,[true,false])!;
        PumpUntil(()=>surface.HasPrompt||playCheck.IsCompleted);Check(surface.HasPrompt,"Play rechecks a dismissed stale-plugin warning");surface.Activate("plugin_prompt:play");Finish(playCheck);Check(playCheck.Result,"Play anyway preserves original bypass choice");
        surface.Activate("nav:mode-prev");Status("mod plugin needs linking");
        playCheck=(Task<bool>)typeof(MainWindow).GetMethod("PrepareExternalChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,[true,false])!;
        Finish(playCheck);Check(playCheck.Result&&!surface.HasPrompt,"vanilla preflight ignores mod-only linking work");
        Status("game changed",vanilla:"runtime newer than game");
        playCheck=(Task<bool>)typeof(MainWindow).GetMethod("PrepareExternalChanges",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,[true,false])!;
        PumpUntil(()=>surface.HasPrompt||playCheck.IsCompleted);Check(surface.HasPrompt,"vanilla play still asks to rebuild an updated game");surface.Activate("plugin_prompt:cancel");Finish(playCheck);Check(!playCheck.Result,"cancel prevents starting stale vanilla game");surface.Activate("nav:mode-prev");
        Status();Directory.CreateDirectory(p.PathIn("mods","offline-added"));File.WriteAllText(p.PathIn("fail-enable"),"");
        check=Invoke(window,"CheckExternalChanges",false,true);PumpUntil(()=>surface.HasPrompt||check.IsCompleted);Check(surface.HasPrompt,"reopening detects an offline-added mod");surface.Activate("newmods:build");Finish(check);
        Check(changes.Scan().Contains("offline-added"),"failed mod build is not acknowledged");surface.Activate("modal:yes");File.Delete(p.PathIn("fail-enable"));
        check=Invoke(window,"CheckExternalChanges",false,true);PumpUntil(()=>surface.HasPrompt||check.IsCompleted);surface.Activate("newmods:build");Finish(check);
        Check(changes.Scan().Length==0,"retry enables and remembers externally added mod only after success");
        Status(refresh:"close while prompting");check=Invoke(window,"CheckExternalChanges",false,true);PumpUntil(()=>surface.HasPrompt||check.IsCompleted);window.Close();Finish(check);Check(!surface.HasPrompt,"closing resolves pending prompt without launching work");
    }
}
