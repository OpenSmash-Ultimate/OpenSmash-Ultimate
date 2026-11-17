// Ported from main's tests/test_launcher_yuzu_ui.cs (the WinForms YuzuRoomPage) for the painted rooms view.
using System.Diagnostics;
using System.Reflection;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Launcher;

static class RoomsChecks
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);Console.WriteLine("PASS "+message);}
    static void Until(Func<bool> condition,string message){var until=DateTime.UtcNow.AddSeconds(60);while(!condition()&&DateTime.UtcNow<until){Dispatcher.UIThread.RunJobs();Thread.Sleep(20);}Check(condition(),message);}
    public static void Run(string temporary)
    {
        string repo=Path.Combine(temporary,"rooms");Directory.CreateDirectory(Path.Combine(repo,"tools"));Directory.CreateDirectory(Path.Combine(repo,"runtime"));File.WriteAllText(Path.Combine(repo,"tools","os_mods.py"),"");
        var settings=new YuzuRoomSettings{Server="room.example:24872",Nickname="TestGuest"};
        Check(settings.Validate()==null,"valid room settings");
        Check(settings.Validate(new string('密',43))!=null,"room password UTF-8 byte limit");
        Check(settings.Validate("x\0y")!=null,"null room password rejected");
        foreach(string server in new[]{"","bad host","room:0","room:65536","room:abc","-bad\nargument"}){settings.Server=server;Check(settings.Validate()!=null,"invalid room server "+server.Replace('\n',' '));}
        settings.Server="room.example:24872";settings.Save(repo);
        Check(YuzuRoomSettings.Load(repo).Server==settings.Server&&YuzuRoomSettings.Load(repo).Profile=="eden","room settings round trip");
        Check(!File.ReadAllText(YuzuRoomSettings.ConfigPath(repo)).Contains("password",StringComparison.OrdinalIgnoreCase),"room configuration has no password property");
        File.WriteAllText(YuzuRoomSettings.ConfigPath(repo),"{\"server\":\"old.example\",\"nickname\":\"Legacy\"}");
        Check(YuzuRoomSettings.Load(repo).Profile=="eden","old room config defaults to Eden");
        settings.Save(repo);
        Environment.SetEnvironmentVariable("SSBU_YUZU_TOKEN","inherited-token");Environment.SetEnvironmentVariable("SSBU_NET_HOST","1");
        var psi=settings.StartInfo(repo,"secret-for-this-launch");
        Environment.SetEnvironmentVariable("SSBU_YUZU_TOKEN",null);Environment.SetEnvironmentVariable("SSBU_NET_HOST",null);
        Check(!string.Join(" ",psi.ArgumentList).Contains("secret-for-this-launch"),"room password absent from process arguments");
        Check(psi.Environment["SSBU_YUZU_PASSWORD"]=="secret-for-this-launch","room password in child environment only");
        Check(!psi.Environment.ContainsKey("SSBU_YUZU_TOKEN")&&!psi.Environment.ContainsKey("SSBU_NET_HOST"),"inherited room backend and token removed");
        Check(psi.ArgumentList.Contains("-Gui")&&psi.ArgumentList.Contains("-NoPrompt"),"noninteractive structured room launch");
        Check(YuzuRoomLaunch.RoomStatus("[yuzu] tx udp anything")==null,"packet trace is not a connection state");
        Check(YuzuRoomLaunch.RoomStatus("[yuzu] joined room as Guest")!.StartsWith("Connected"),"room admission shown");
        Check(YuzuRoomLaunch.RoomStatus("[yuzu] state: connection lost; retrying")!.Contains("retrying"),"reconnect state replaces connected state");
        Check(YuzuRoomLaunch.RoomStatus("[yuzu] wrong room password")!.Contains("rejected"),"room password rejection shown");
        Directory.CreateDirectory(Path.Combine(repo,"build"));
        File.WriteAllText(Path.Combine(repo,"build","yuzu-client.log"),"[yuzu] wrong room password\n"+new string('x',150000)+"\n[gfx] window closed\n");
        Check(YuzuRoomLaunch.EarlyExitMessage(repo).Contains("password"),"early refusal survives long boot logs");
        File.Delete(Path.Combine(repo,"build","yuzu-client.log"));
        Check(YuzuRoomLaunch.EarlyExitMessage(repo).Contains("Connection log"),"early exit without log has actionable fallback");

        var window=new MainWindow([],repo,enableBackgroundActions:false);var surface=window.Surface;
        var type=typeof(OpenSmash.Visuals.ClassicSurface);const BindingFlags flags=BindingFlags.Instance|BindingFlags.NonPublic;
        surface.Navigate("settings");
        var tabs=(Array)type.GetField("_settingsTabs",flags)!.GetValue(surface)!;
        string last=(string)tabs.GetValue(tabs.Length-1)!.GetType().GetField("Name")!.GetValue(tabs.GetValue(tabs.Length-1))!;
        if(!OperatingSystem.IsWindows()){Check(last!="Advanced","room entry hidden off Windows");window.Close();return;}
        Check(last=="Advanced","room lives under the last settings tab");
        type.GetField("_settingsTab",flags)!.SetValue(surface,tabs.Length-1);
        type.GetMethod("HandleSettingsKey",flags)!.Invoke(surface,[Key.Enter]);
        Check(surface.CurrentView.EndsWith("rooms"),"Enter opens the optional room from Advanced settings");
        Check(surface.RoomField("password").PasswordChar!=default,"room password masked");
        Check(surface.RoomProfile=="eden"&&surface.RoomField("server").Text=="room.example:24872","saved room settings loaded into the view");
        Check(surface.RoomField("server").IsVisible,"room inputs shown on the rooms view");
        surface.RoomField("server").Text="bad host";surface.Activate("rooms:connect");Dispatcher.UIThread.RunJobs();
        Check(surface.RoomStatus.Contains("Enter a server")&&surface.CurrentView.EndsWith("rooms"),"invalid connect stays on the page");
        surface.RoomField("server").Text="new.example:5000";surface.Activate("rooms:legacy");surface.RoomField("password").Text="not-saved";surface.Activate("rooms:save");
        Check(YuzuRoomSettings.Load(repo).Profile=="yuzu","room view saves the selected profile");
        Check(!File.ReadAllText(YuzuRoomSettings.ConfigPath(repo)).Contains("not-saved"),"room view never saves the password");
        string script=Path.Combine(repo,"tools","yuzu_client.ps1");
        File.WriteAllText(script,"param([switch]$Gui,[switch]$NoPrompt,[switch]$CheckOnly,[string]$Server,[string]$Nickname,[string]$ProxyProfile)\nif ($CheckOnly) { Write-Output 'ready'; exit 0 }\nexit 3\n");
        surface.Activate("rooms:check");
        Until(()=>!surface.RoomBusy,"room setup check completes asynchronously");
        Check(surface.RoomStatus.StartsWith("Setup is ready"),"setup check does not claim a connection");
        File.WriteAllText(script,"Write-Error 'fixture preparation failed'; exit 7");
        surface.Activate("rooms:check");
        Until(()=>!surface.RoomBusy,"failed room preparation releases busy state");
        Check(surface.RoomStatus.StartsWith("Could not start"),"room startup error displayed in the page");
        window.Show();
        using(var frame=window.CaptureRenderedFrame())Check(frame!=null,"renders the rooms view");
        surface.Activate("nav:home");
        Check(!surface.RoomField("server").IsVisible,"room inputs hidden on the home page");
        window.Close();
        var fake=new ProcessStartInfo("powershell.exe"){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
        foreach(string a in new[]{"-NoProfile","-Command","Write-Output 'YUZU_RESULT {\"pid\":"+Environment.ProcessId+"}'"})fake.ArgumentList.Add(a);
        fake.Environment["SSBU_YUZU_PASSWORD"]="secret";
        using Process? handedOff=Task.Run(()=>YuzuRoomLaunch.RunAsync(fake,_=>{})).GetAwaiter().GetResult();
        Check(handedOff?.Id==Environment.ProcessId&&!fake.Environment.ContainsKey("SSBU_YUZU_PASSWORD"),"structured room result parsed and secret released");
    }
}
